#pragma once

#include <stdbool.h>

/* Called from the WebSocket task whenever the relay connection comes up or
   drops. Do not block; post to a queue if you need to update the display. */
typedef void (*relay_status_cb_t)(bool connected);

/* Open (and keep open) the WebSocket to the relay. Reconnects automatically.
   relay_url is the https:// base URL from the config partition; name/secret
   identify this device to the relay. Call once, after WiFi has an IP and the
   local web server is running — proxied requests are answered by making
   loopback HTTP requests to it. */
void start_relay_client(const char *relay_url, const char *device_name,
                        const char *device_secret, relay_status_cb_t status_cb);
