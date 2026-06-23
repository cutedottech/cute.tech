# firmware/

ESP-IDF **v6.x** project for the cute.tech device. Targets two boards (both ESP32-S3):
- **Heltec LoRa ESP32-S3** — has built-in SSD1306 OLED. Build with `sdkconfig.heltec`.
- **Seeed Studio XIAO ESP32-S3 Sense** — no OLED, tiny form factor. Build with `sdkconfig.xiao`.

Board is selected at build time via Kconfig (`CONFIG_BOARD_HELTEC` / `CONFIG_BOARD_XIAO`). OLED display code is compiled out on XIAO.

## What this firmware does

The device is a tiny web server that:
1. Reads its config (device name, secret, relay URL) from NVS flash
2. Connects to WiFi via Improv WiFi (provisioned at flash time, not hardcoded)
3. Serves a user-editable webpage at `/` (stored in NVS, editable via `/edit`)
4. Maintains a WebSocket connection to the relay so the page is reachable at `name.cute.tech`

## Feature checklist

**Config / boot**
- [ ] Read NVS namespace `config`: keys `device_name`, `device_secret`, `relay_url`
- [ ] Improv WiFi serial protocol support (for browser-based WiFi provisioning)
- [ ] OLED status display: connecting → got IP → relay connected (or "not configured")

**HTTP server**
- [ ] `GET /` — serve HTML from NVS (fallback to default if not set)
- [ ] `GET /edit` — serve editor UI (baked into firmware as embedded file)
- [ ] `GET /raw` — return current HTML as plain text
- [ ] `POST /save` — accept new HTML, store to NVS (add size limit ~8KB)
- [ ] `GET /status` — JSON: device_name, uptime_s, free_heap, ip, chip_model

**Relay client**
- [ ] WebSocket client: `wss://<relay_url>/_ws?device=<name>&key=<secret>`
- [ ] Route relay requests to same handlers as HTTP server
- [ ] Reconnect with exponential backoff on disconnect

**Stretch**
- [ ] `GET /gpio/:pin` / `POST /gpio/:pin` — digital read/write
- [ ] Peripheral discovery

## Key constraints
- **No secrets in source code.** WiFi creds come from Improv WiFi at flash time. Device name + secret come from an NVS partition programmed at flash time. Config lives only in NVS.
- Max HTML size: 8 KB (NVS blob limit is well within this; keep it tight for memory)
- Target chip: ESP32-S3, IDF v5.x

## Reference
The `CuteTech Workshop/` folder at the repo root is a working prototype using this same architecture. Read it for patterns but do not copy secrets or hardcoded values.

## Building
```bash
cd firmware
idf.py set-target esp32s3

# Heltec LoRa ESP32-S3:
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.heltec build

# Seeed XIAO ESP32-S3 Sense:
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.xiao build

idf.py -p /dev/tty.usbserial-* flash monitor
```
