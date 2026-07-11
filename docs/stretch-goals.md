# Stretch goals

Ideas that fit the project but aren't needed for the workshop to run.

## Edit your device in vscode.dev

A VS Code *web extension* that exposes a device as a virtual filesystem, so
participants can open vscode.dev and edit their microcontroller's files like a
normal folder — full editor, zero installs.

How it works:
- The extension registers a `FileSystemProvider` for a `cutefs://` scheme and
  maps it onto the device's existing HTTP API (all reachable through the relay):
  `readDirectory` → `GET /api/files`, `readFile` → `GET /api/file?path=`,
  `writeFile` → `PUT /api/file?path=&key=`, `delete` → `DELETE /api/file`.
- Web extensions run in a browser worker (no Node APIs), but `fetch` is
  available, which is all this needs.

What it requires:
- **CORS**: the extension fetches from the vscode.dev origin, so device API
  responses need `Access-Control-Allow-Origin` + `OPTIONS` preflight handling.
  Easiest added once in relay/worker.js where the device response is
  reconstructed, rather than in firmware.
- **Marketplace publishing**: vscode.dev can only install extensions from the
  VS Code Marketplace (or open-vsx) — one-time chore. There's a localhost
  sideload flow for development.
- Optionally a URL handler so a link like `vscode.dev/+cute.cutefs/name` opens
  a device directly — the flash page could show it after flashing succeeds.

Simpler precursor (also undone): a dependency-free sync CLI (`pull` / `push` /
`watch`) against the same API, for people who want their local editor. Files
cap at 256 KB (`MAX_UPLOAD` in firmware/main/server.c) and the relay handles
one request at a time, so uploads must be sequential.

## Hardware API — pages that can script their own peripherals

Because a participant's page is served by the device itself, page JS can call
the device's API same-origin: hardware access from their own site is just
`fetch()`. Expose a hardware namespace on the firmware HTTP server:

- `GET/POST /api/pin/{n}` — Arduino-style digital read/write, `?mode=in|in_pullup|out`
- `GET /api/touch/{n}` — ESP32-S3 native capacitive touch (GPIO 1–14; XIAO pads are 1–9)
- `GET /api/temp` — internal temperature sensor
- `GET/POST /api/led` — user LED (GPIO 21 on XIAO)
- `GET /api/photo` — OV2640 JPEG capture on XIAO Sense (espressif/esp32-camera component)
- `GET /api/features` — boot-time capability detection (camera/OLED/touch), same
  single-binary philosophy as the OLED probe in display.c

Plus an embedded `/cute.js` helper (like editor.html) so pages write
`cute.led(true)`, `await cute.touch(3)`, `img.src = await cute.photo()`.

Constraints & decisions:
- Relay is JSON-text frames, one request at a time, 10 s timeout: single photos
  need a `body_b64` field in relay_client.c + worker.js (~33% inflation, fine
  with PSRAM); live MJPEG streaming is local-network only. Cap photos ~VGA.
- Reads + LED public (mischief is a feature); raw pin writes gated by the
  `?key=` edit password and a per-board safe-pin allowlist (avoid strapping,
  flash/PSRAM, camera/mic pins).


