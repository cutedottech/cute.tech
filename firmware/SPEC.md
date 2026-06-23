# cute.tech firmware spec

Target boards (both use ESP32-S3 chip):
- **Heltec LoRa ESP32-S3** — 8MB flash, built-in SSD1306 OLED (128×64) on I2C, LoRa radio
- **Seeed Studio XIAO ESP32-S3 Sense** — 8MB flash, no OLED, camera + microphone, tiny form factor

Framework: **ESP-IDF v6.x** (C)  
Build: `idf.py set-target esp32s3 && idf.py build`

### Board selection via Kconfig

Select the board at build time using a `CONFIG_BOARD_*` option defined in `Kconfig`. The OLED display is Heltec-only — on XIAO the display functions are compiled out and become no-ops. All other firmware features (HTTP server, relay client, Improv WiFi, NVS config) are identical across boards.

```
# Build for Heltec:
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.heltec build

# Build for XIAO:
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.xiao build
```

Board-specific pin assignments (verify against your exact board revision):

| Signal | Heltec LoRa ESP32-S3 | XIAO ESP32-S3 Sense |
|--------|----------------------|---------------------|
| OLED SDA | GPIO 17 | — (no OLED) |
| OLED SCL | GPIO 18 | — (no OLED) |
| OLED RST | GPIO 21 | — |
| Status LED | GPIO 35 (onboard LED) | GPIO 21 (onboard LED) |

---

## Boot sequence

```
power on
  → init NVS
  → init OLED → show "cute.tech" splash
  → read config from NVS (device_name, device_secret, relay_url, wifi_ssid, wifi_pass)
  → if config incomplete:
      show "needs setup" on OLED
      start Improv WiFi serial listener (wait for browser to provision)
  → else:
      connect WiFi → show "connecting: <ssid>" on OLED
      on IP: show IP on OLED, start HTTP server, start relay WS client
      on relay connect: show "online: <device_name>.cute.tech"
```

---

## NVS config

Namespace: `config`

| Key           | Type   | Description                          |
|---------------|--------|--------------------------------------|
| `device_name` | string | e.g. `toaster` — used as WS `?device=` param and displayed on OLED |
| `device_secret` | string | random hex secret, checked by relay |
| `relay_url`   | string | hostname only, e.g. `relay.cute.tech` |
| `wifi_ssid`   | string | WiFi network name |
| `wifi_pass`   | string | WiFi password (empty string = open network) |

Namespace: `webpage`

| Key    | Type | Description                                |
|--------|------|--------------------------------------------|
| `html` | blob | Current user HTML. Max 8 KB. Falls back to embedded `index.html` if absent. |

---

## Improv WiFi provisioning

Improv WiFi is a standard serial protocol — the browser side is handled by esp-web-tools, the firmware side needs to implement it.

Use the **ESP-IDF Improv WiFi component** from the component registry:
```
idf_component.yml:
  dependencies:
    espressif/improv_wifi: "^1.0.0"
```

Flow:
1. On boot with missing config: start listening for Improv WiFi on UART0 (the USB serial port)
2. Browser sends WiFi SSID + password via the Improv protocol
3. Firmware attempts to connect; reports success/failure back via Improv
4. On success: save `wifi_ssid` and `wifi_pass` to NVS
5. After WiFi connects, the browser then programs the NVS partition with `device_name`, `device_secret`, `relay_url` via a second WebSerial step (separate from Improv)

> **Note:** Improv WiFi only provisions WiFi. The device name/secret arrive via a separate NVS partition flashed by the browser. Firmware should handle both being set independently and only go "online" when all five NVS keys are present.

---

## HTTP server

Start after WiFi connects. Max URI handlers: 8. Listens on port 80.

### `GET /`
Returns the user's custom HTML.
- Read blob from NVS `webpage/html`
- If absent, return embedded `index.html` (compiled in via `EMBED_FILES`)
- `Content-Type: text/html; charset=UTF-8`

### `GET /raw`
Returns the current HTML as plain text (used by the editor to load content).
- Same source as `GET /` 
- `Content-Type: text/plain; charset=UTF-8`

### `GET /edit`
Returns the editor UI (compiled in via `EMBED_FILES` from `editor.html`).
- `Content-Type: text/html; charset=UTF-8`
- Editor fetches `/raw` on load, POSTs to `/save` on save

### `POST /save`
Accepts new HTML and saves to NVS.
- Read body up to 8192 bytes; reject with 400 if `Content-Length` is 0 or > 8192
- Save to NVS `webpage/html`
- Respond `200 OK` with body `OK`
- Respond `500` if NVS write fails

### `GET /status`
Returns device info as JSON.
```json
{
  "device_name": "toaster",
  "relay_url": "relay.cute.tech",
  "uptime_s": 3600,
  "free_heap": 180000,
  "ip": "192.168.1.42",
  "chip_model": "ESP32-S3"
}
```
- `Content-Type: application/json`

---

## Relay WebSocket client

Connect to: `wss://<relay_url>/_ws?device=<device_name>&key=<device_secret>`

Uses `esp_websocket_client` with `esp_crt_bundle_attach` for TLS.

### Incoming message format (relay → device)
```json
{ "id": 42, "method": "GET", "path": "/", "body": "" }
```

### Outgoing message format (device → relay)
```json
{ "id": 42, "status": 200, "ct": "text/html", "body": "<html>..." }
```

The `body` field must be JSON-escaped (escape `"`, `\`, `\n`, `\r`, `\t`).

### Routing
Map `path` to the same handlers as the HTTP server:

| Path    | Handler          |
|---------|-----------------|
| `/`     | root (custom HTML) |
| `/raw`  | raw HTML        |
| `/edit` | editor UI       |
| `/save` | save HTML (method must be POST) |
| `/status` | device status JSON |
| anything else | 404 `Not found` |

Strip query strings before matching (e.g. `/edit?foo=bar` → `/edit`).

### Reconnection
- On disconnect: reconnect with exponential backoff, starting at 1s, doubling each attempt, capped at 30s
- `esp_websocket_client` handles reconnection internally with `reconnect_timeout_ms`; set this to 5000

---

## OLED display

Heltec ESP32-S3's OLED is a 128×64 SSD1306 on I2C. Use the `heltec_unofficial` component or configure I2C manually (SDA=17, SCL=18 on Heltec LoRa ESP32-S3 — verify against your board revision).

Run all display calls in a dedicated FreeRTOS task (I2C is too slow for the event loop).

States to show:

| State | Display |
|-------|---------|
| Boot | `cute.tech` |
| Needs setup | `needs setup` / `open cute.tech` |
| Connecting WiFi | `connecting...` + SSID (truncated to 12 chars) |
| Got IP | IP address |
| Relay connected | `online` + device name |
| Relay disconnected | `reconnecting...` |

---

## Memory / size constraints

- Max HTML blob: 8 KB (NVS blob max is 508 KB but keep it small for heap)
- WS send buffer: allocate dynamically, free immediately after send
- Heap watchdog: if `esp_get_free_heap_size() < 30000`, log a warning (don't reboot — leave that for later)

---

## What NOT to hardcode

- WiFi credentials — come from NVS only
- Device name / secret — come from NVS only  
- Relay URL — come from NVS only
- No `#define WORKER_SECRET` or similar anywhere in source

---

## Reference

The working prototype is at `CuteTech Workshop/hello_world/main/hello_world_main.c` (gitignored, local only).  
It shows the HTTP handler pattern, NVS read/write, WebSocket client setup, and JSON escape helper.  
Reuse the patterns but do not copy hardcoded values.
