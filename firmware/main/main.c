/*
 * cute.tech firmware — ESP32-S3
 *
 * See SPEC.md for the full feature spec before implementing anything.
 * See CLAUDE.md for build instructions and board targets.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    printf("cute.tech firmware — not yet implemented\n");
    printf("See firmware/SPEC.md for the feature spec.\n");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
