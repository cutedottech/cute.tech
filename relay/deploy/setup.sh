#!/usr/bin/env bash
# One-shot setup for the cute.tech relay on a fresh Ubuntu 24.04 server.
#
# Usage: copy the relay/ folder to the server, then:
#   sudo ./deploy/setup.sh
#
# Safe to re-run — it skips what's already installed and re-copies the
# worker code + configs, so it doubles as a deploy script for updates.
set -euo pipefail

RELAY_DIR=/opt/cute-relay
ENV_FILE=/etc/cute-relay.env
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ "$(id -u)" -ne 0 ]; then
  echo "Run with sudo." >&2
  exit 1
fi

# ── 1. workerd binary ────────────────────────────────────────────────────────
if ! command -v workerd >/dev/null; then
  echo "Installing workerd..."
  curl -fsSL https://github.com/cloudflare/workerd/releases/latest/download/workerd-linux-64.gz \
    | gunzip > /usr/local/bin/workerd
  chmod +x /usr/local/bin/workerd
fi
echo "workerd: $(workerd --version)"

# ── 2. Caddy (official apt repo) ─────────────────────────────────────────────
if ! command -v caddy >/dev/null; then
  echo "Installing Caddy..."
  apt-get update
  apt-get install -y debian-keyring debian-archive-keyring apt-transport-https curl
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
    | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
    > /etc/apt/sources.list.d/caddy-stable.list
  apt-get update
  apt-get install -y caddy
fi

# The apt build doesn't include DNS provider plugins; the wildcard cert needs
# the Cloudflare one for its DNS-01 challenge. `add-package` rebuilds the
# binary in place via Caddy's own package server and keeps the apt-installed
# systemd unit, user, and log setup intact.
if ! caddy list-modules 2>/dev/null | grep -q '^dns.providers.cloudflare$'; then
  echo "Adding Cloudflare DNS module to Caddy..."
  caddy add-package github.com/caddy-dns/cloudflare
fi

# Cloudflare API token for the DNS-01 challenge — scoped to Zone:DNS:Edit for
# the cute.tech zone only. Can't be generated like ADMIN_TOKEN; create it at
# https://dash.cloudflare.com/profile/api-tokens and drop it in below.
CF_ENV_FILE=/etc/caddy/cloudflare.env
if [ ! -f "$CF_ENV_FILE" ]; then
  cat > "$CF_ENV_FILE" <<'EOF'
# Cloudflare API token, scoped to Zone:DNS:Edit for the cute.tech zone only.
# Create one at https://dash.cloudflare.com/profile/api-tokens
CF_API_TOKEN=
EOF
  chmod 600 "$CF_ENV_FILE"
  echo "Created $CF_ENV_FILE — add your Cloudflare API token, then re-run this script." >&2
  exit 1
fi
if ! grep -q '^CF_API_TOKEN=.\+' "$CF_ENV_FILE"; then
  echo "$CF_ENV_FILE exists but CF_API_TOKEN is empty — add it, then re-run." >&2
  exit 1
fi

mkdir -p /etc/systemd/system/caddy.service.d
cat > /etc/systemd/system/caddy.service.d/cloudflare.conf <<EOF
[Service]
EnvironmentFile=$CF_ENV_FILE
EOF

# ── 3. Relay user + files ────────────────────────────────────────────────────
id -u cute-relay >/dev/null 2>&1 || \
  useradd --system --home "$RELAY_DIR" --shell /usr/sbin/nologin cute-relay

mkdir -p "$RELAY_DIR" "$RELAY_DIR/data"   # data/ = persisted device registry
cp "$SRC_DIR/worker.js" "$SRC_DIR/config.capnp" "$RELAY_DIR/"
chown -R cute-relay:cute-relay "$RELAY_DIR"

# ── 4. Admin token (generated once, kept across re-runs) ────────────────────
# Human-readable so it can be read off a projector at a workshop.
# Rotate anytime with: sudo ./deploy/new-token.sh --install
if [ ! -f "$ENV_FILE" ]; then
  echo "ADMIN_TOKEN=$("$SRC_DIR/deploy/new-token.sh")" > "$ENV_FILE"
  chmod 600 "$ENV_FILE"
  echo "Generated new ADMIN_TOKEN in $ENV_FILE"
fi

# ── 5. Services ──────────────────────────────────────────────────────────────
cp "$SRC_DIR/deploy/Caddyfile" /etc/caddy/Caddyfile
cp "$SRC_DIR/deploy/cute-relay.service" /etc/systemd/system/cute-relay.service

systemctl daemon-reload
systemctl enable --now cute-relay
systemctl restart cute-relay   # pick up new worker.js on re-runs
systemctl restart caddy   # not reload — the cloudflare.conf drop-in needs a fresh process

echo
echo "── Done ─────────────────────────────────────────────────────────────"
systemctl --no-pager --lines=0 status cute-relay caddy | grep -E "●|Active:"
echo
echo "Admin token (needed by the flash tool to register devices):"
grep ADMIN_TOKEN "$ENV_FILE"
echo
echo "Smoke test:  curl -s https://relay.cute.tech/status"
