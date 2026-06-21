# web/

GitHub Pages site for cute.tech. Served from the `main` branch `/web` folder at `cute.tech`.

## Pages

- `index.html` — landing page (existing, pixel-art aesthetic with Press Start 2P font)
- `flash.html` — WebSerial firmware flash tool (to build)

## Flash tool flow (`flash.html`)

1. User plugs in ESP32-S3 via USB, clicks "Install"
2. [esp-web-tools](https://esphome.github.io/esp-web-tools/) flashes firmware from `firmware/manifest.json`
3. Improv WiFi dialog: user enters DWEB WiFi SSID + password, sent over serial
4. User types their chosen device name (e.g. "toaster")
5. Browser generates a random secret
6. Browser POSTs `{name, secret}` to relay `/register` endpoint
7. Browser programs NVS config partition (name + secret + relay URL) via WebSerial
8. Device reboots → connects relay → device is live at `name.cute.tech`

## Firmware manifest

`firmware/manifest.json` — referenced by esp-web-tools. Points to pre-built `.bin` files.
Bins are committed to the repo so GitHub Pages can serve them statically.

## Style

Match the existing landing page: `#fef9f0` background, dotted grid, Press Start 2P font, pixel aesthetic. Keep it cute.

## Deployment

GitHub Pages: Settings → Pages → Deploy from branch `main`, folder `/web`. No build step — plain HTML/CSS/JS.
