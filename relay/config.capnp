using Workerd = import "/workerd/workerd.capnp";

# cute.tech relay — workerd config
# Run locally: npm run dev  (sets a dev ADMIN_TOKEN)
# Production: Caddy terminates TLS and proxies here; see deploy/setup.sh.
#
# ADMIN_TOKEN is read from the environment — workerd must be started with
# ADMIN_TOKEN set (deploy/cute-relay.service loads it from /etc/cute-relay.env).

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .mainWorker),
    # Writable directory the worker mirrors the device registry into
    # (data/devices.json), so restarts don't strand registered devices.
    # Path is relative to workerd's working directory: /opt/cute-relay in
    # production (systemd WorkingDirectory), relay/ for npm run dev.
    (name = "registry-disk", disk = (path = "data", writable = true)),
  ],
  sockets = [
    # Plain HTTP, loopback only — Caddy (or local curl) is the only client.
    (name = "http", address = "127.0.0.1:8080", http = (), service = "main"),
  ],
);

const mainWorker :Workerd.Worker = (
  modules = [
    (name = "worker.js", esModule = embed "worker.js"),
  ],
  compatibilityDate = "2025-01-01",
  durableObjectNamespaces = [
    (className = "RelayObject", uniqueKey = "cute-tech-relay"),
  ],
  durableObjectStorage = (inMemory = void),
  bindings = [
    (name = "RELAY", durableObjectNamespace = "RelayObject"),
    (name = "ADMIN_TOKEN", fromEnvironment = "ADMIN_TOKEN"),
    (name = "REGISTRY", service = "registry-disk"),
  ],
);
