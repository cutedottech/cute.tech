# web/

GitHub Pages site for cute.tech. Served from the `main` branch `/web` folder at `cute.tech`.

## Pages

- `index.html` — landing page (existing, pixel-art aesthetic with Press Start 2P font)
- `flash.html` — WebSerial firmware flash tool (to build)

## Flash tool flow (`flash.html`)

1. User plugs in ESP32-S3 via USB, clicks "flash firmware"
2. [esp-web-tools](https://esphome.github.io/esp-web-tools/) flashes firmware from `firmware/manifest.json`
3. User enters WiFi SSID + password, device name, an edit password (protects the device's editing endpoints; checked by firmware as `?key=` query param — headers don't survive the relay), and the workshop code (= relay `ADMIN_TOKEN`; prefillable via `flash.html#code=...` link fragment)
4. Browser generates a random 64-hex-char secret
5. Browser POSTs `{name, secret}` to relay `/register` (Bearer workshop code)
6. Browser writes the raw `config` partition at 0x310000 via esptool.js — packed `device_config_t` struct (magic + ssid + password + name + relay URL + secret; layout comment in flash.html must match firmware/main/main.c)
7. Device reboots → WiFi → opens WSS to relay → live at `name.cute.tech`

## Firmware manifest

`firmware/manifest.json` — referenced by esp-web-tools. Points to pre-built `.bin` files.
Bins are committed to the repo so GitHub Pages can serve them statically.

## Style

Match the existing landing page: `#fef9f0` background, dotted grid, Press Start 2P font, pixel aesthetic. Keep it cute.

## Deployment

GitHub Pages via `.github/workflows/pages.yml`, which publishes the `web/` folder on every push to `main` that touches it. (Branch-based Pages only supports `/` or `/docs`, so the folder must be deployed through Actions.) Repo setting: Settings → Pages → Source = "GitHub Actions". No build step — plain HTML/CSS/JS.
