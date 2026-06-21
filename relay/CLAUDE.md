# relay/

Multi-tenant WebSocket relay server. Runs on [workerd](https://github.com/cloudflare/workerd) — Cloudflare's open-source Workers runtime — so it can run on any VPS without a Cloudflare account, using the same code.

## What it does

Each cute.tech device opens a persistent outbound WebSocket to `/_ws?device=<name>&key=<secret>`. When a browser visits `name.cute.tech`, the relay proxies the HTTP request over that socket and returns the device's response.

Each device gets its own in-memory Durable Object instance. Last device to connect for a given name wins (previous is disconnected) — intentional; it's a DWEB provocation about name ownership.

## Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET *` | `name.cute.tech/*` | — | Proxy to named device |
| `GET /_ws` | `?device=name&key=secret` | device secret | Device WebSocket upgrade |
| `POST /register` | — | `Authorization: Bearer <ADMIN_TOKEN>` | Register a device name + secret |
| `GET /status` | — | — | List currently online device names |

## Device registry

Devices are registered dynamically via `POST /register`. For a workshop, the browser flash tool calls this automatically after generating the device secret. The registry lives in memory (resets on restart) — fine for a workshop. For persistent deployment, write to a `devices.json` file on disk.

## Running locally

```bash
npm install
npx workerd serve config.capnp
```

## Deployment (VPS)

1. Install workerd on the VPS
2. Put nginx in front for TLS termination (Let's Encrypt wildcard cert for `*.cute.tech`)
3. DNS: add `*.cute.tech A <VPS IP>` wildcard record
4. Run workerd as a systemd service

## Secrets

`ADMIN_TOKEN` — set in environment / Cap'n Proto config. Never commit. Used to protect the `/register` endpoint.
