#include "relay_client.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "cJSON.h"

/* Bridges the relay to the local web server. The relay sends one JSON text
   message per browser request: {id, method, path, body}. We replay it as a
   loopback HTTP request to our own server on 127.0.0.1 and send back
   {id, status, ct, body}. Requests are handled one at a time, in order —
   fine at workshop scale, and the id field means ordering never matters. */

static const char *TAG = "relay";

#define MAX_REQUEST_JSON  (48 * 1024)  /* incoming message cap (editor saves) */
#define MAX_RESPONSE_BODY (24 * 1024)  /* per-response cap relayed back */
#define LOCAL_URL_MAX     600

static esp_websocket_client_handle_t s_client;
static relay_status_cb_t s_status_cb;

/* Reassembly buffer: one WS text message may arrive split across several
   DATA events (payload_offset tells us where each chunk goes). */
static char  *s_msg;
static size_t s_msg_len;

/* ── loopback HTTP ───────────────────────────────────────────────────────── */

typedef struct {
    char ct[96];
} resp_meta_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        strcasecmp(evt->header_key, "Content-Type") == 0) {
        strlcpy(((resp_meta_t *)evt->user_data)->ct, evt->header_value,
                sizeof(((resp_meta_t *)evt->user_data)->ct));
    }
    return ESP_OK;
}

static esp_http_client_method_t method_from(const char *m)
{
    if (!strcmp(m, "PUT"))    return HTTP_METHOD_PUT;
    if (!strcmp(m, "POST"))   return HTTP_METHOD_POST;
    if (!strcmp(m, "DELETE")) return HTTP_METHOD_DELETE;
    if (!strcmp(m, "HEAD"))   return HTTP_METHOD_HEAD;
    return HTTP_METHOD_GET;
}

/* JSON text frames must be valid UTF-8, so binary bodies can't travel over
   this relay. Text-ish content types pass through; anything else gets an
   honest apology instead of a corrupted response. */
static bool is_text_type(const char *ct)
{
    return !strncmp(ct, "text/", 5) ||
           !strncmp(ct, "application/json", 16) ||
           !strncmp(ct, "application/javascript", 22) ||
           !strncmp(ct, "image/svg", 9);
}

/* Replay one relayed request against the local server. Returns the HTTP
   status; fills *body_out (malloc'd, caller frees) and meta->ct. */
static int loopback_request(const char *method, const char *path,
                            const char *req_body, char **body_out,
                            resp_meta_t *meta)
{
    char url[LOCAL_URL_MAX];
    if (snprintf(url, sizeof(url), "http://127.0.0.1:80%s", path) >= (int)sizeof(url))
        return 414;

    esp_http_client_config_t cfg = {
        .url           = url,
        .timeout_ms    = 8000,          /* relay gives up at 10 s */
        .event_handler = http_event_cb,
        .user_data     = meta,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return 500;

    esp_http_client_set_method(http, method_from(method));

    size_t req_len = req_body ? strlen(req_body) : 0;
    int status = 502;
    if (esp_http_client_open(http, req_len) != ESP_OK) goto done;
    if (req_len && esp_http_client_write(http, req_body, req_len) < (int)req_len)
        goto done;
    if (esp_http_client_fetch_headers(http) < 0) goto done;
    status = esp_http_client_get_status_code(http);

    char  *buf  = malloc(MAX_RESPONSE_BODY + 1);
    size_t used = 0;
    if (buf) {
        int r;
        while (used < MAX_RESPONSE_BODY &&
               (r = esp_http_client_read(http, buf + used, MAX_RESPONSE_BODY - used)) > 0)
            used += r;
        buf[used] = '\0';
        if (used == MAX_RESPONSE_BODY)
            ESP_LOGW(TAG, "response for %s truncated at %d bytes", path, MAX_RESPONSE_BODY);
        *body_out = buf;
    }

done:
    esp_http_client_cleanup(http);
    return status;
}

/* ── request handling ────────────────────────────────────────────────────── */

static void handle_message(const char *json, size_t len)
{
    cJSON *req = cJSON_ParseWithLength(json, len);
    if (!req) {
        ESP_LOGW(TAG, "unparseable message from relay (%u bytes)", (unsigned)len);
        return;
    }

    cJSON *jid   = cJSON_GetObjectItem(req, "id");
    cJSON *jmeth = cJSON_GetObjectItem(req, "method");
    cJSON *jpath = cJSON_GetObjectItem(req, "path");
    cJSON *jbody = cJSON_GetObjectItem(req, "body");

    const char *method = cJSON_IsString(jmeth) ? jmeth->valuestring : "GET";
    const char *path   = cJSON_IsString(jpath) ? jpath->valuestring : "/";
    const char *body   = cJSON_IsString(jbody) ? jbody->valuestring : NULL;

    if (!cJSON_IsNumber(jid) || path[0] != '/') {
        cJSON_Delete(req);
        return;
    }

    resp_meta_t meta = {0};
    char *resp_body = NULL;
    int status = loopback_request(method, path, body, &resp_body, &meta);

    const char *ct = meta.ct[0] ? meta.ct : "text/plain";
    const char *out_body = resp_body ? resp_body : "";
    if (resp_body && *resp_body && !is_text_type(ct)) {
        status   = 200;
        ct       = "text/plain";
        out_body = "this file is binary — the cute.tech relay only carries "
                   "text for now. visit the device on its local network to "
                   "see it in full.";
    }

    cJSON *rep = cJSON_CreateObject();
    cJSON_AddNumberToObject(rep, "id", jid->valuedouble);
    cJSON_AddNumberToObject(rep, "status", status);
    cJSON_AddStringToObject(rep, "ct", ct);
    cJSON_AddStringToObject(rep, "body", out_body);
    char *out = cJSON_PrintUnformatted(rep);

    if (out) {
        if (esp_websocket_client_send_text(s_client, out, strlen(out),
                                           pdMS_TO_TICKS(10000)) < 0)
            ESP_LOGW(TAG, "failed to send response for id %d", (int)jid->valuedouble);
        cJSON_free(out);
    }

    /* Log without the query string — it can carry the edit password */
    ESP_LOGI(TAG, "%s %.*s -> %d", method, (int)strcspn(path, "?"), path, status);

    cJSON_Delete(rep);
    cJSON_Delete(req);
    free(resp_body);
}

/* ── WebSocket events ────────────────────────────────────────────────────── */

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to relay");
        if (s_status_cb) s_status_cb(true);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "relay connection lost — will retry");
        free(s_msg);
        s_msg = NULL;
        if (s_status_cb) s_status_cb(false);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code != 0x1 && data->op_code != 0x0)
            break;  /* ignore ping/pong/close frames */
        if (data->payload_len <= 0 || data->payload_len > MAX_REQUEST_JSON) {
            ESP_LOGW(TAG, "dropping oversized message (%d bytes)", data->payload_len);
            break;
        }
        if (data->payload_offset == 0) {
            free(s_msg);
            s_msg = malloc(data->payload_len);
            s_msg_len = 0;
        }
        if (!s_msg) break;
        memcpy(s_msg + data->payload_offset, data->data_ptr, data->data_len);
        s_msg_len = data->payload_offset + data->data_len;
        if ((int)s_msg_len >= data->payload_len) {
            handle_message(s_msg, s_msg_len);
            free(s_msg);
            s_msg = NULL;
        }
        break;
    }
}

/* ── public API ──────────────────────────────────────────────────────────── */

void start_relay_client(const char *relay_url, const char *device_name,
                        const char *device_secret, relay_status_cb_t status_cb)
{
    /* https://host -> wss://host/_ws?device=..&key=..  (http -> ws for dev) */
    const char *host = relay_url;
    const char *scheme = "wss";
    if (!strncmp(relay_url, "https://", 8))     host = relay_url + 8;
    else if (!strncmp(relay_url, "http://", 7)) { host = relay_url + 7; scheme = "ws"; }

    static char uri[320];
    snprintf(uri, sizeof(uri), "%s://%s/_ws?device=%s&key=%s",
             scheme, host, device_name, device_secret);

    s_status_cb = status_cb;

    esp_websocket_client_config_t cfg = {
        .uri                  = uri,
        .crt_bundle_attach    = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        /* Keepalive well under proxy/NAT idle timeouts (~100 s) */
        .ping_interval_sec    = 20,
        .buffer_size          = 4096,
        .task_stack           = 8192,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "websocket client init failed");
        return;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_client);
    ESP_LOGI(TAG, "connecting to %s://%s/_ws as \"%s\"", scheme, host, device_name);
}
