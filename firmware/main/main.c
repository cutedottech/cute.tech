/*
 * cute.tech firmware — ESP32-S3 (Heltec LoRa ESP32-S3)
 *
 * Boot → NVS config → Improv WiFi serial (if unconfigured) → HTTP server + WS relay
 *
 * Architecture:
 *   - All config (WiFi creds, device name/secret/relay) lives in NVS, never hardcoded
 *   - Improv WiFi serial protocol spoken directly on UART0 (USB serial)
 *   - HTTP server serves /, /raw, /edit, /save, /status on port 80
 *   - WebSocket relay client connects to wss://<relay_url>/_ws?device=<name>&key=<secret>
 *   - OLED display driven from a queue-based FreeRTOS task (I2C is too slow for event loop)
 */

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_http_server.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "display.h"

static const char *TAG = "cute.tech";

/* ── Embedded HTML files (baked in at build time) ────────────────────────── */

extern const char index_html_start[]  asm("_binary_index_html_start");
extern const char editor_html_start[] asm("_binary_editor_html_start");

/* ── NVS config ──────────────────────────────────────────────────────────── */

#define CFG_NS     "config"
#define CFG_NAME   "device_name"
#define CFG_SECRET "device_secret"
#define CFG_RELAY  "relay_url"
#define CFG_SSID   "wifi_ssid"
#define CFG_PASS   "wifi_pass"

#define WEB_NS     "webpage"
#define WEB_KEY    "html"
#define MAX_HTML   8192

static char g_device_name[64]    = {0};
static char g_device_secret[128] = {0};
static char g_relay_url[128]     = {0};
static char g_wifi_ssid[33]      = {0};
static char g_wifi_pass[65]      = {0};

/* Returns true if all five required keys are present and non-empty */
static bool config_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) return false;

    bool ok = true;
    size_t n;

    n = sizeof(g_device_name);
    if (nvs_get_str(h, CFG_NAME, g_device_name, &n) != ESP_OK || !g_device_name[0]) ok = false;

    n = sizeof(g_device_secret);
    if (nvs_get_str(h, CFG_SECRET, g_device_secret, &n) != ESP_OK || !g_device_secret[0]) ok = false;

    n = sizeof(g_relay_url);
    if (nvs_get_str(h, CFG_RELAY, g_relay_url, &n) != ESP_OK || !g_relay_url[0]) ok = false;

    n = sizeof(g_wifi_ssid);
    if (nvs_get_str(h, CFG_SSID, g_wifi_ssid, &n) != ESP_OK || !g_wifi_ssid[0]) ok = false;

    /* wifi_pass may legitimately be empty (open network) */
    n = sizeof(g_wifi_pass);
    if (nvs_get_str(h, CFG_PASS, g_wifi_pass, &n) != ESP_OK) g_wifi_pass[0] = '\0';

    nvs_close(h);
    return ok;
}

static esp_err_t config_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, CFG_SSID, ssid);
    nvs_set_str(h, CFG_PASS, pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));
        strlcpy(g_wifi_pass, pass ? pass : "", sizeof(g_wifi_pass));
    }
    return err;
}

/* ── Display task ────────────────────────────────────────────────────────── */

typedef enum {
    DISP_SPLASH,
    DISP_NEEDS_SETUP,
    DISP_CONNECTING,
    DISP_CONNECTED,
    DISP_DISCONNECTED,
    DISP_ONLINE,
    DISP_RECONNECTING,
} disp_cmd_t;

typedef struct {
    disp_cmd_t cmd;
    char       arg[32];
    int        reason;
} disp_msg_t;

static QueueHandle_t s_disp_q;

static void disp_post(disp_cmd_t cmd, const char *arg, int reason)
{
    disp_msg_t m = { .cmd = cmd, .reason = reason };
    if (arg) strlcpy(m.arg, arg, sizeof(m.arg));
    xQueueSend(s_disp_q, &m, 0);
}

static void display_task(void *pv)
{
    disp_msg_t m;
    for (;;) {
        if (xQueueReceive(s_disp_q, &m, portMAX_DELAY)) {
            switch (m.cmd) {
                case DISP_SPLASH:       display_show_splash();               break;
                case DISP_NEEDS_SETUP:  display_show_needs_setup();          break;
                case DISP_CONNECTING:   display_show_connecting(m.arg);      break;
                case DISP_CONNECTED:    display_show_connected(m.arg);       break;
                case DISP_DISCONNECTED: display_show_disconnected(m.reason); break;
                case DISP_ONLINE:       display_show_online(m.arg);          break;
                case DISP_RECONNECTING: display_show_reconnecting();         break;
            }
        }
    }
}

/* ── HTML storage ────────────────────────────────────────────────────────── */

static char g_html[MAX_HTML];

static void html_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(g_html);
    esp_err_t err = nvs_open(WEB_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, WEB_KEY, g_html, &len);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        strlcpy(g_html, index_html_start, sizeof(g_html));
        ESP_LOGI(TAG, "Using default HTML");
    } else {
        ESP_LOGI(TAG, "Loaded HTML from NVS (%d bytes)", (int)len);
    }
}

static esp_err_t html_save(const char *html)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WEB_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, WEB_KEY, html, strlen(html) + 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── JSON helpers ────────────────────────────────────────────────────────── */

/* JSON-escape src into dst. Returns bytes written (excluding NUL). */
static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 3 < dst_size; i++) {
        char c = src[i];
        if      (c == '"')  { dst[j++] = '\\'; dst[j++] = '"';  }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n';  }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r';  }
        else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't';  }
        else                { dst[j++] = c; }
    }
    dst[j] = '\0';
    return j;
}

static int json_get_int(const char *json, const char *key)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    return atoi(p + strlen(needle));
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_size)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && *(p + 1)) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/* ── HTTP handlers ───────────────────────────────────────────────────────── */

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send(req, g_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handler_raw(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=UTF-8");
    httpd_resp_send(req, g_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handler_edit(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send(req, editor_html_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > MAX_HTML - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            total <= 0 ? "No content" : "Content too large");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, g_html + received, total - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        received += n;
    }
    g_html[received] = '\0';

    esp_err_t err = html_save(g_html);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTML updated (%d bytes)", received);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static void fill_status_json(char *buf, size_t buf_size)
{
    char ip_str[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }
    snprintf(buf, buf_size,
        "{\"device_name\":\"%s\","
        "\"relay_url\":\"%s\","
        "\"uptime_s\":%lld,"
        "\"free_heap\":%lu,"
        "\"ip\":\"%s\","
        "\"chip_model\":\"ESP32-S3\"}",
        g_device_name, g_relay_url,
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size(),
        ip_str);
}

static esp_err_t handler_status(httpd_req_t *req)
{
    char json[320];
    fill_status_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_webserver(void)
{
    static httpd_handle_t server = NULL;
    if (server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    static const httpd_uri_t routes[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = handler_root   },
        { .uri = "/raw",    .method = HTTP_GET,  .handler = handler_raw    },
        { .uri = "/edit",   .method = HTTP_GET,  .handler = handler_edit   },
        { .uri = "/save",   .method = HTTP_POST, .handler = handler_save   },
        { .uri = "/status", .method = HTTP_GET,  .handler = handler_status },
    };
    for (int i = 0; i < 5; i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "HTTP server started");
}

/* ── WebSocket relay client ──────────────────────────────────────────────── */

static esp_websocket_client_handle_t s_ws_client = NULL;

static void ws_send_response(int id, int status, const char *ct, const char *body)
{
    if (!body) body = "";
    size_t body_len = strlen(body);
    /* 6× worst case: every char becomes \\uXXXX (but we only escape 5 chars) */
    size_t esc_size = body_len * 2 + 2;
    char *esc = malloc(esc_size);
    if (!esc) return;
    json_escape(body, esc, esc_size);

    size_t buf_size = esc_size + strlen(ct) + 64;
    char *json = malloc(buf_size);
    if (!json) { free(esc); return; }
    snprintf(json, buf_size,
             "{\"id\":%d,\"status\":%d,\"ct\":\"%s\",\"body\":\"%s\"}",
             id, status, ct, esc);
    free(esc);

    if (esp_get_free_heap_size() < 30000)
        ESP_LOGW(TAG, "Low heap: %lu bytes free", (unsigned long)esp_get_free_heap_size());

    esp_websocket_client_send_text(s_ws_client, json, strlen(json), portMAX_DELAY);
    free(json);
}

static void ws_route(int id, const char *method, const char *path, const char *body)
{
    if (strcmp(path, "/") == 0) {
        ws_send_response(id, 200, "text/html; charset=UTF-8", g_html);
    } else if (strcmp(path, "/raw") == 0) {
        ws_send_response(id, 200, "text/plain; charset=UTF-8", g_html);
    } else if (strcmp(path, "/edit") == 0) {
        ws_send_response(id, 200, "text/html; charset=UTF-8", editor_html_start);
    } else if (strcmp(path, "/status") == 0) {
        char status_json[320];
        fill_status_json(status_json, sizeof(status_json));
        ws_send_response(id, 200, "application/json", status_json);
    } else if (strcmp(path, "/save") == 0 && strcmp(method, "POST") == 0) {
        if (!body || !body[0] || strlen(body) > (size_t)(MAX_HTML - 1)) {
            ws_send_response(id, 400, "text/plain", "Bad request");
            return;
        }
        strlcpy(g_html, body, sizeof(g_html));
        esp_err_t err = html_save(g_html);
        ws_send_response(id, err == ESP_OK ? 200 : 500,
                         "text/plain", err == ESP_OK ? "OK" : "NVS error");
    } else {
        ws_send_response(id, 404, "text/plain", "Not found");
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Relay connected");
            disp_post(DISP_ONLINE, g_device_name, 0);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Relay disconnected — will retry");
            disp_post(DISP_RECONNECTING, NULL, 0);
            break;

        case WEBSOCKET_EVENT_DATA:
            /* Only handle complete, unfragmented text frames */
            if (data->op_code != 0x01 || data->payload_offset != 0 ||
                data->data_len != data->payload_len || data->data_len <= 0) break;
            {
                char *buf = strndup(data->data_ptr, data->data_len);
                if (!buf) break;

                int  id     = json_get_int(buf, "id");
                char path[128]       = "/";
                char method[8]       = "GET";
                char body[MAX_HTML]  = {0};

                json_get_str(buf, "path",   path,   sizeof(path));
                json_get_str(buf, "method", method, sizeof(method));
                json_get_str(buf, "body",   body,   sizeof(body));
                free(buf);

                if (id < 0) break;

                /* Strip query string before routing */
                char *qs = strchr(path, '?');
                if (qs) *qs = '\0';

                ws_route(id, method, path, body);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "Relay WebSocket error");
            break;

        default:
            break;
    }
}

static void ws_client_start(void)
{
    if (s_ws_client) return; /* already started; component auto-reconnects */

    char uri[256];
    snprintf(uri, sizeof(uri), "wss://%s/_ws?device=%s&key=%s",
             g_relay_url, g_device_name, g_device_secret);

    esp_websocket_client_config_t ws_cfg = {
        .uri                  = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        .crt_bundle_attach    = esp_crt_bundle_attach,
    };
    s_ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);
    esp_websocket_client_start(s_ws_client);
    ESP_LOGI(TAG, "Relay client → %s", uri);
}

/* ── WiFi ────────────────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        disp_post(DISP_CONNECTING, g_wifi_ssid, 0);
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = event_data;
        ESP_LOGW(TAG, "WiFi disconnected (reason %d) — retrying", disc->reason);
        disp_post(DISP_DISCONNECTED, NULL, disc->reason);
        /* Small delay before retry prevents rapid reconnect storms */
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
        disp_post(DISP_CONNECTED, ip_str, 0);
        html_load();
        start_webserver();
        ws_client_start();
    }
}

/* Shared WiFi init — call once, then call esp_wifi_start() */
static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

static void wifi_connect_from_nvs(void)
{
    wifi_init_sta();

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     g_wifi_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, g_wifi_pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.pmf_cfg.capable  = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* STA_START → wifi_event_handler → esp_wifi_connect() */
}

/* ── Improv WiFi serial provisioning ─────────────────────────────────────── */
/*
 * Improv WiFi is a lightweight serial protocol for provisioning WiFi over USB.
 * The browser side is handled by esp-web-tools. We speak it directly here.
 *
 * Packet format: IMPROV\x01<type><len><data...><checksum>
 *   type 0x03 = RPC command from browser
 *   type 0x01 = state update to browser
 *   type 0x02 = error update to browser
 *   type 0x04 = RPC result to browser
 *
 * RPC command 0x01 = send WiFi credentials {ssid_len, ssid..., pass_len, pass...}
 *
 * States:  0x01=Awaiting Authorization, 0x02=Authorized, 0x03=Provisioning, 0x04=Provisioned
 * Errors:  0x00=None, 0x01=Invalid RPC packet, 0x03=Unable to connect
 */

#define IMPROV_UART        UART_NUM_0
#define IMPROV_BAUD        115200
#define IMPROV_BUF_SIZE    256
#define IMPROV_RPC_WIFI    0x01

static const uint8_t IMPROV_HEADER[] = {'I','M','P','R','O','V'};

static uint8_t improv_checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

static void improv_send(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
    /* Header(6) + version(1) + type(1) + len(1) + payload + checksum(1) */
    uint8_t pkt[IMPROV_BUF_SIZE];
    size_t  pos = 0;
    memcpy(pkt, IMPROV_HEADER, 6); pos += 6;
    pkt[pos++] = 0x01;           /* version */
    pkt[pos++] = type;
    pkt[pos++] = payload_len;
    memcpy(pkt + pos, payload, payload_len); pos += payload_len;
    pkt[pos++] = improv_checksum(pkt, pos);
    uart_write_bytes(IMPROV_UART, (char *)pkt, pos);
}

static void improv_send_state(uint8_t state)
{
    improv_send(0x01, &state, 1);
}

static void improv_send_error(uint8_t err)
{
    improv_send(0x02, &err, 1);
}

/* Attempt WiFi connect; return true and write IP on success */
static bool improv_try_wifi(const char *ssid, const char *pass, char *ip_out, size_t ip_out_size)
{
    /* If WiFi stack not yet up, init it now */
    static bool wifi_stack_up = false;
    if (!wifi_stack_up) {
        wifi_init_sta();
        wifi_stack_up = true;
    }

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.pmf_cfg.capable  = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();
    esp_wifi_connect();

    /* Wait up to 15 s for an IP */
    for (int i = 0; i < 150; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!netif) continue;
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr) {
            snprintf(ip_out, ip_out_size, IPSTR, IP2STR(&ip_info.ip));
            return true;
        }
    }
    return false;
}

/* Process one complete Improv RPC packet starting at buf[0] */
static void improv_handle_rpc(const uint8_t *data, uint8_t data_len)
{
    if (data_len < 2) { improv_send_error(0x01); return; }

    uint8_t cmd = data[0];
    /* data[1] is payload length, data[2..] is payload */

    if (cmd != IMPROV_RPC_WIFI) {
        improv_send_error(0x01); /* unknown command */
        return;
    }

    /* WiFi credentials command: ssid_len, ssid..., pass_len, pass... */
    const uint8_t *p   = data + 2;
    const uint8_t *end = data + data_len;

    if (p >= end) { improv_send_error(0x01); return; }
    uint8_t ssid_len = *p++;
    if (p + ssid_len > end) { improv_send_error(0x01); return; }

    char ssid[33] = {0};
    memcpy(ssid, p, ssid_len < 32 ? ssid_len : 32);
    p += ssid_len;

    char pass[65] = {0};
    if (p < end) {
        uint8_t pass_len = *p++;
        if (p + pass_len <= end) {
            memcpy(pass, p, pass_len < 64 ? pass_len : 64);
        }
    }

    ESP_LOGI(TAG, "Improv: provisioning SSID=\"%s\"", ssid);
    improv_send_state(0x03); /* Provisioning */
    disp_post(DISP_CONNECTING, ssid, 0);

    char ip_str[16] = {0};
    if (improv_try_wifi(ssid, pass, ip_str, sizeof(ip_str))) {
        config_save_wifi(ssid, pass);
        ESP_LOGI(TAG, "Improv: provisioned, IP=%s", ip_str);

        /* RPC result: list of strings [redirect_url] */
        char url[64];
        snprintf(url, sizeof(url), "http://%s", ip_str);
        uint8_t result[70];
        result[0] = IMPROV_RPC_WIFI;
        result[1] = (uint8_t)(strlen(url) + 1);      /* 1 string */
        result[2] = (uint8_t)strlen(url);
        memcpy(result + 3, url, strlen(url));
        improv_send(0x04, result, 3 + strlen(url));

        improv_send_state(0x04); /* Provisioned */
        disp_post(DISP_CONNECTED, ip_str, 0);

        /* Start serving so the user can reach the device immediately */
        html_load();
        start_webserver();
        /* Relay won't start until device_name/secret are also programmed */
        if (g_device_name[0] && g_device_secret[0] && g_relay_url[0])
            ws_client_start();
    } else {
        ESP_LOGW(TAG, "Improv: WiFi connect failed");
        improv_send_error(0x03); /* Unable to connect */
        improv_send_state(0x02); /* Back to Authorized */
    }
}

/* Improv WiFi serial listener task.
   Runs when config is incomplete — parks here parsing UART0 packets. */
static void improv_task(void *pv)
{
    uart_config_t uart_cfg = {
        .baud_rate  = IMPROV_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(IMPROV_UART, IMPROV_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(IMPROV_UART, &uart_cfg);

    ESP_LOGI(TAG, "Improv WiFi: listening on UART0...");
    disp_post(DISP_NEEDS_SETUP, NULL, 0);
    improv_send_state(0x02); /* Authorized (no auth step needed) */

    uint8_t buf[IMPROV_BUF_SIZE];
    size_t  buf_len = 0;

    for (;;) {
        uint8_t byte;
        int n = uart_read_bytes(IMPROV_UART, &byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        buf[buf_len++] = byte;

        /* Scan for a complete IMPROV packet in the buffer */
        while (buf_len >= 10) {  /* minimum: 6 header + ver + type + len + checksum */
            /* Find "IMPROV" header */
            size_t start = 0;
            while (start + 5 < buf_len &&
                   memcmp(buf + start, IMPROV_HEADER, 6) != 0) start++;
            if (start > 0) {
                /* Shift out bytes before the header */
                memmove(buf, buf + start, buf_len - start);
                buf_len -= start;
                continue;
            }
            if (buf_len < 10) break;

            /* buf[6] = version, buf[7] = type, buf[8] = length */
            uint8_t pkt_type = buf[7];
            uint8_t pkt_len  = buf[8];
            size_t  pkt_total = 9 + pkt_len + 1; /* header+ver+type+len + data + checksum */

            if (buf_len < pkt_total) break; /* wait for more bytes */

            /* Verify checksum */
            uint8_t expected = improv_checksum(buf, pkt_total - 1);
            if (buf[pkt_total - 1] != expected) {
                /* Bad checksum — consume one byte and retry */
                memmove(buf, buf + 1, --buf_len);
                continue;
            }

            if (pkt_type == 0x03) { /* RPC command */
                improv_handle_rpc(buf + 9, pkt_len);
            }
            /* Consume this packet */
            memmove(buf, buf + pkt_total, buf_len - pkt_total);
            buf_len -= pkt_total;
        }

        if (buf_len >= IMPROV_BUF_SIZE) buf_len = 0; /* overflow guard */
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_disp_q = xQueueCreate(4, sizeof(disp_msg_t));
    display_init();
    xTaskCreate(display_task, "display", 4096, NULL, 5, NULL);
    disp_post(DISP_SPLASH, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(1000)); /* let the splash be visible */

    if (config_load()) {
        ESP_LOGI(TAG, "Config: name=%s relay=%s ssid=%s",
                 g_device_name, g_relay_url, g_wifi_ssid);
        wifi_connect_from_nvs();
    } else {
        /* Some keys are missing — wait for browser to provision via Improv + NVS flash */
        ESP_LOGW(TAG, "Config incomplete — starting Improv WiFi provisioning");
        xTaskCreate(improv_task, "improv", 4096, NULL, 4, NULL);
    }
}
