#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "display.h"
#include "server.h"
#include "relay_client.h"

static const char *TAG = "main";

/* ── Config partition ────────────────────────────────────────────────────── */

#define CONFIG_MAGIC 0x45545543u  /* "CUTE" little-endian */

typedef struct {
    uint32_t magic;
    char     ssid[64];
    char     password[64];
    char     device_name[33];
    char     relay_url[128];
    char     device_secret[65];
    char     edit_password[33];
} __attribute__((packed)) device_config_t;

static device_config_t s_cfg;

static bool load_config(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "config");
    if (!part) {
        ESP_LOGW(TAG, "config partition not found");
        return false;
    }
    esp_err_t err = esp_partition_read(part, 0, &s_cfg, sizeof(s_cfg));
    if (err != ESP_OK || s_cfg.magic != CONFIG_MAGIC) {
        ESP_LOGW(TAG, "config partition empty or invalid (magic=0x%08lx)", s_cfg.magic);
        return false;
    }
    s_cfg.ssid[sizeof(s_cfg.ssid) - 1]             = '\0';
    s_cfg.password[sizeof(s_cfg.password) - 1]     = '\0';
    s_cfg.device_name[sizeof(s_cfg.device_name)-1] = '\0';
    s_cfg.relay_url[sizeof(s_cfg.relay_url) - 1]   = '\0';
    s_cfg.device_secret[sizeof(s_cfg.device_secret) - 1] = '\0';
    s_cfg.edit_password[sizeof(s_cfg.edit_password) - 1] = '\0';
    ESP_LOGI(TAG, "config loaded: ssid=\"%s\" name=\"%s\"", s_cfg.ssid, s_cfg.device_name);
    return true;
}

/* ── Display task ────────────────────────────────────────────────────────── */

typedef enum { DISP_CONNECTING, DISP_CONNECTED, DISP_DISCONNECTED, DISP_ONLINE } disp_cmd_t;

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

static void display_task(void *arg)
{
    disp_msg_t m;
    for (;;) {
        if (xQueueReceive(s_disp_q, &m, portMAX_DELAY)) {
            switch (m.cmd) {
                case DISP_CONNECTING:   display_show_connecting(m.arg);     break;
                case DISP_CONNECTED:    display_show_connected(m.arg);      break;
                case DISP_DISCONNECTED: display_show_disconnected(m.reason); break;
                case DISP_ONLINE:       display_show_online(m.arg);         break;
            }
        }
    }
}

/* ── WiFi ────────────────────────────────────────────────────────────────── */

static esp_timer_handle_t s_rescan_timer;

static void start_scan(void)
{
    ESP_LOGI(TAG, "scanning for \"%s\"...", s_cfg.ssid);
    wifi_scan_config_t scan_cfg = {
        .show_hidden          = true,
        .scan_type            = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    esp_wifi_scan_start(&scan_cfg, false);
}

static void rescan_timer_cb(void *arg) { start_scan(); }

static void connect(void)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid,     s_cfg.ssid,     sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_cfg.password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode  = s_cfg.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable     = true;
    cfg.sta.pmf_cfg.required    = false;
    disp_post(DISP_CONNECTING, s_cfg.ssid, 0);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    esp_wifi_connect();
}

static void on_scan_done(void)
{
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);

    wifi_ap_record_t *aps = malloc(num * sizeof(wifi_ap_record_t));
    if (!aps) {
        esp_wifi_scan_get_ap_records(&num, NULL);
        esp_timer_start_once(s_rescan_timer, 5000000);
        return;
    }
    esp_wifi_scan_get_ap_records(&num, aps);

    bool found = false;
    for (int i = 0; i < num; i++) {
        if (strcmp((char *)aps[i].ssid, s_cfg.ssid) == 0) {
            found = true;
            break;
        }
    }
    free(aps);

    if (found) {
        connect();
    } else {
        ESP_LOGW(TAG, "\"%s\" not visible — retrying in 5 s", s_cfg.ssid);
        esp_timer_start_once(s_rescan_timer, 5000000);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        start_scan();
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        on_scan_done();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = event_data;
        ESP_LOGW(TAG, "disconnected (reason %d) — rescanning in 3 s", disc->reason);
        disp_post(DISP_DISCONNECTED, NULL, disc->reason);
        esp_timer_start_once(s_rescan_timer, 3000000);
    }
}

/* Runs on the WebSocket task — just posts to the display queue. */
static void relay_status_cb(bool connected)
{
    if (connected) disp_post(DISP_ONLINE, s_cfg.device_name, 0);
    /* On drop: leave the last screen up; the client reconnects on its own
       and WiFi loss already triggers DISP_DISCONNECTED. */
}

/* TLS certificate checks need a roughly-correct clock, and we boot in 1970.
   Wait for SNTP before opening the WSS connection; on timeout start anyway
   (the client retries, so a late sync still recovers). Runs in its own task
   because the wait must not block the event loop. */
static void relay_start_task(void *arg)
{
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK)
        ESP_LOGW(TAG, "SNTP sync timed out — TLS may fail until time is set");

    start_relay_client(s_cfg.relay_url, s_cfg.device_name,
                       s_cfg.device_secret, relay_status_cb);
    vTaskDelete(NULL);
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "got IP: %s", ip);
        disp_post(DISP_CONNECTED, ip, 0);

        /* Server and relay client persist across WiFi drops — start once. */
        static bool s_services_started = false;
        if (s_services_started) return;
        s_services_started = true;

        start_webserver(s_cfg.edit_password);
        if (s_cfg.relay_url[0] && s_cfg.device_secret[0]) {
            xTaskCreate(relay_start_task, "relay_start", 4096, NULL, 5, NULL);
        } else {
            ESP_LOGW(TAG, "no relay configured — local-only mode");
        }
    }
}

static void wifi_init(void)
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

    esp_timer_create_args_t timer_args = { .callback = rescan_timer_cb, .name = "rescan" };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_rescan_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_country_t country = { .cc = "DE", .schan = 1, .nchan = 13,
                                .policy = WIFI_COUNTRY_POLICY_MANUAL };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    ESP_ERROR_CHECK(esp_wifi_start());
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

    storage_init();

    if (!load_config()) {
        display_show_no_config();
        /* Halt — nothing useful to do without credentials */
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    wifi_init();
}
