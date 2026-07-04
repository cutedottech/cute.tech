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
