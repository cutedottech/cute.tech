// cute.tech multi-tenant relay
// Each device gets its own Durable Object instance keyed by device name.
// Devices connect via WebSocket; browser requests are proxied over that socket.
//
// Endpoints:
//   GET  /_ws?device=<name>&key=<secret>   Device WebSocket upgrade
//   POST /register  {name, secret}          Register a device (admin token required)
//   GET  /status                            List online device names
//   GET  /ring?to=next|prev|random          Webring hop: 302 to a neighbouring
//                                           online device (current device from
//                                           Host subdomain, or ?me= override)
//   GET  <anything else>                    Proxy to device named by Host subdomain

// Device registry: name → secret. Module-level because workerd bindings are
// read-only config, not mutable state. Mirrored to data/devices.json via the
// REGISTRY disk service so restarts don't strand registered devices.
const devices = new Map();
let registryLoad = null;

function loadRegistry(env) {
  registryLoad ??= (async () => {
    try {
      const res = await env.REGISTRY.fetch("http://registry/devices.json");
      if (res.status === 404) return; // first boot, nothing saved yet
      if (!res.ok) throw new Error(`disk read failed: ${res.status}`);
      for (const [name, secret] of Object.entries(await res.json())) {
        devices.set(name, secret);
      }
    } catch (err) {
      // Unreadable/corrupt file: serve with an empty registry rather than
      // going down — devices can re-register.
      console.error("registry load failed:", err.message ?? err);
    }
  })();
  return registryLoad;
}

async function saveRegistry(env) {
  const res = await env.REGISTRY.fetch("http://registry/devices.json", {
    method: "PUT",
    body: JSON.stringify(Object.fromEntries(devices)),
  });
  if (!res.ok) throw new Error(`disk write failed: ${res.status}`);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    await loadRegistry(env);

    // Registration endpoint. The flash page calls this from another origin
    // (cute.tech or localhost), so it needs CORS headers — and the
    // Authorization header makes browsers send a preflight OPTIONS first.
    if (url.pathname === "/register" && request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }
    if (url.pathname === "/register" && request.method === "POST") {
      return handleRegister(request, env);
    }

    // Status endpoint
    if (url.pathname === "/status" && request.method === "GET") {
      return handleStatus(request, env);
    }

    // Webring hop. Path-based like /status, so it works on every device
    // subdomain and device pages can use plain relative links:
    // <a href="/ring?to=next">. This reserves the /ring path on device sites.
    if (url.pathname === "/ring" && request.method === "GET") {
      return handleRing(request, env, url);
    }

    // Device WebSocket upgrade
    if (url.pathname === "/_ws") {
      const deviceName = url.searchParams.get("device") ?? "";
      const key = url.searchParams.get("key") ?? "";

      const secret = devices.get(deviceName);
      if (!secret) {
        return new Response("Unknown device", { status: 404 });
      }
      if (!timingSafeEqual(key, secret)) {
        return new Response("Unauthorized", { status: 403 });
      }

      const id = env.RELAY.idFromName(deviceName);
      return env.RELAY.get(id).fetch(request);
    }

    // Proxy: device name from subdomain (e.g. toaster.cute.tech → "toaster")
    const host = request.headers.get("host") ?? "";
    const deviceName = host.split(".")[0];

    if (!devices.has(deviceName)) {
      return new Response("No device registered with this name", { status: 404 });
    }

    const id = env.RELAY.idFromName(deviceName);
    return env.RELAY.get(id).fetch(request);
  },
};

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
  "Access-Control-Allow-Headers": "Authorization, Content-Type",
};

async function handleRegister(request, env) {
  const token = (request.headers.get("Authorization") ?? "").replace("Bearer ", "");
  if (!env.ADMIN_TOKEN || !timingSafeEqual(token, env.ADMIN_TOKEN)) {
    return new Response("Unauthorized", { status: 403, headers: CORS_HEADERS });
  }

  let body;
  try {
    body = await request.json();
  } catch {
    return new Response("Invalid JSON", { status: 400, headers: CORS_HEADERS });
  }

  const { name, secret } = body;
  if (!name || !secret || typeof name !== "string" || typeof secret !== "string") {
    return new Response("name and secret required", { status: 400, headers: CORS_HEADERS });
  }
  if (!/^[a-z0-9-]{1,32}$/.test(name)) {
    return new Response("name must be lowercase alphanumeric/hyphens, max 32 chars",
      { status: 400, headers: CORS_HEADERS });
  }

  devices.set(name, secret);
  try {
    await saveRegistry(env);
  } catch (err) {
    // The in-memory registration still works until the next restart, and
    // failing here would block the participant mid-flash. Log and carry on.
    console.error("registry save failed:", err.message ?? err);
  }
  return new Response(JSON.stringify({ ok: true, name }), {
    headers: { "Content-Type": "application/json", ...CORS_HEADERS },
  });
}

// Ask each registered device's DO whether it has a live connection.
// For simplicity: list all registered names and mark which DOs respond online.
// The DO exposes a /_status internal path we check. Sorted so the webring
// order is stable.
async function onlineDevices(env) {
  const names = [...devices.keys()];
  const online = [];

  await Promise.all(names.map(async (name) => {
    try {
      const id = env.RELAY.idFromName(name);
      const res = await env.RELAY.get(id).fetch(
        new Request("http://internal/_status")
      );
      if (res.status === 200) online.push(name);
    } catch {
      // DO not reachable or no connection — skip
    }
  }));

  return online.sort();
}

async function handleStatus(request, env) {
  return new Response(JSON.stringify({ online: await onlineDevices(env) }), {
    headers: {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": "*",
    },
  });
}

// The ring is the alphabetical list of currently-online devices. Offline
// devices and visitors without one (e.g. arriving from cute.tech) enter the
// ring at its edge instead of erroring — a webring should never dead-end.
async function handleRing(request, env, url) {
  const host = request.headers.get("host") ?? "";
  const me = url.searchParams.get("me") ?? host.split(".")[0];
  const to = url.searchParams.get("to") ?? "next";

  const ring = await onlineDevices(env);
  const others = ring.filter((n) => n !== me);
  if (others.length === 0) {
    // Nobody else online — the ring is just you (or empty). Back to home.
    return Response.redirect("https://cute.tech/", 302);
  }

  let dest;
  const i = ring.indexOf(me);
  if (to === "random") {
    dest = others[Math.floor(Math.random() * others.length)];
  } else if (to === "prev") {
    dest = i >= 0 ? ring[(i - 1 + ring.length) % ring.length] : ring[ring.length - 1];
  } else {
    dest = i >= 0 ? ring[(i + 1) % ring.length] : ring[0];
  }
  return Response.redirect(`https://${dest}.cute.tech/`, 302);
}

// ── Durable Object ────────────────────────────────────────────────────────────

export class RelayObject {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.esp = null;
    this.pending = new Map();
    this.nextId = 1;
  }

  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === "/_status") {
      return new Response(this.esp ? "online" : "offline", {
        status: this.esp ? 200 : 503,
      });
    }

    if (url.pathname === "/_ws") {
      return this.#handleDeviceUpgrade(request);
    }

    return this.#relay(request, url);
  }

  #handleDeviceUpgrade(request) {
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("Expected WebSocket upgrade", { status: 426 });
    }

    const [client, server] = Object.values(new WebSocketPair());
    server.accept();

    if (this.esp) {
      try { this.esp.close(1001, "replaced"); } catch (_) {}
    }
    this.esp = server;

    server.addEventListener("message", (evt) => {
      let msg;
      try { msg = JSON.parse(evt.data); } catch (_) { return; }
      const waiter = this.pending.get(msg.id);
      if (waiter) {
        this.pending.delete(msg.id);
        waiter.resolve(msg);
      }
    });

    server.addEventListener("close", () => {
      if (this.esp === server) this.esp = null;
      for (const [id, waiter] of this.pending) {
        this.pending.delete(id);
        waiter.reject(new Error("device disconnected"));
      }
    });

    server.addEventListener("error", () => {
      if (this.esp === server) this.esp = null;
    });

    return new Response(null, { status: 101, webSocket: client });
  }

  async #relay(request, url) {
    if (!this.esp) {
      return new Response("Device not connected", { status: 503 });
    }

    const id = this.nextId++;
    const body = request.method !== "GET" && request.method !== "HEAD"
      ? await request.text()
      : "";

    const msg = JSON.stringify({ id, method: request.method, path: url.pathname + url.search, body });

    const responsePromise = new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
    });

    try {
      this.esp.send(msg);
    } catch {
      this.pending.delete(id);
      return new Response("Failed to reach device", { status: 502 });
    }

    let deviceResponse;
    try {
      deviceResponse = await Promise.race([
        responsePromise,
        sleep(10_000).then(() => { throw new Error("timeout"); }),
      ]);
    } catch (err) {
      this.pending.delete(id);
      return new Response(
        err.message === "timeout" ? "Device timed out" : "Device disconnected",
        { status: err.message === "timeout" ? 504 : 503 }
      );
    }

    return new Response(deviceResponse.body ?? "", {
      status: deviceResponse.status ?? 200,
      headers: { "Content-Type": deviceResponse.ct ?? "text/plain" },
    });
  }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function timingSafeEqual(a, b) {
  if (a.length !== b.length) {
    let diff = 0;
    for (let i = 0; i < Math.max(a.length, b.length); i++) diff |= 1;
    return false;
  }
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}
