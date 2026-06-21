using Workerd = import "/workerd/workerd.capnp";

# cute.tech relay — workerd config
# Run locally: npx workerd serve config.capnp
# For production: put nginx in front for TLS, run workerd as a systemd service.
#
# ADMIN_TOKEN binding below is a placeholder — replace with a real secret
# or set via environment variable / secret manager before deploying.

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .mainWorker),
  ],
  sockets = [
    # Local/dev: plain HTTP on :8080
    # For production behind nginx TLS termination, this stays as HTTP
    (name = "http", address = "*:8080", http = (), service = "main"),
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
    # ADMIN_TOKEN: set this to a strong random secret before deploying
    # Generate one: openssl rand -hex 32
    (name = "ADMIN_TOKEN", text = "REPLACE_ME"),
  ],
);
