# cute.tech

A mono-repo for the cute.tech workshop project. Participants flash ESP32-S3 devices via a browser, give them a subdomain on cute.tech, and end up with a self-editable website served from a microcontroller.

## Sub-projects

| Folder | What it is | Context file |
|--------|-----------|-------------|
| `web/` | GitHub Pages site — landing page + WebSerial flash tool | `web/CLAUDE.md` |
| `firmware/` | ESP-IDF project for the ESP32-S3 device | `firmware/CLAUDE.md` |
| `relay/` | workerd relay server — gives each device a public subdomain | `relay/CLAUDE.md` |
| `docs/` | Workshop materials and notes | — |

## Key architecture

Each device opens a persistent outbound WebSocket to the relay. Browser requests to `name.cute.tech` are proxied through that socket to the device, which responds. Devices never accept inbound TCP from the internet.

```
Browser → HTTPS → relay (cute.tech subdomain) → WSS → ESP32
```

## What never goes in this repo
- `CuteTech Workshop/` — prototype/reference, gitignored
- Secrets, passwords, device keys
- ESP-IDF build output (`firmware/build/`)

## Working on a sub-project
Spawn a subagent with the relevant subfolder as its focus and read that folder's CLAUDE.md first. Subagents don't need to know about other subfolders.
