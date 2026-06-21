/*
 * cute.tech firmware — ESP32-S3 (Heltec LoRa ESP32-S3)
 *
 * Features to implement (see firmware/CLAUDE.md for full checklist):
 *   - NVS config: device_name, device_secret, relay_url
 *   - Improv WiFi serial provisioning
 *   - HTTP server: /, /edit, /raw, /save, /status
 *   - WebSocket relay client → wss://<relay_url>/_ws?device=<name>&key=<secret>
 *   - OLED status display
 *
 * Reference: CuteTech Workshop/hello_world/main/hello_world_main.c
 * (in the repo root, gitignored) shows a working implementation of the
 * HTTP server + relay client pattern. Do not copy secrets or hardcoded values.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    printf("cute.tech firmware — not yet implemented\n");
    printf("See firmware/CLAUDE.md for the feature checklist.\n");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
