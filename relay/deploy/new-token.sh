#!/usr/bin/env bash
# Generate a human-readable workshop token, e.g. "fuzzy-otter-disco-42".
#
#   ./new-token.sh            print a new token
#   sudo ./new-token.sh --install
#                             write it into /etc/cute-relay.env and restart
#                             the relay (NOTE: restart wipes the in-memory
#                             device registry — devices must re-register)
#
# Format: adjective-noun-noun-NN. With 64-word lists that's ~26M
# combinations — plenty for a token that only gates subdomain registration
# and gets rotated after each workshop, while staying easy to read off a
# projector and type.
set -euo pipefail

ENV_FILE=/etc/cute-relay.env

ADJ=(
  tiny soft fuzzy pink shiny happy sleepy bouncy round warm cozy silly
  gentle sunny minty peachy bubbly fluffy dizzy jolly perky quirky snug
  plump chirpy zesty breezy dreamy mellow nifty dandy spiffy zippy wiggly
  giggly sparky toasty crispy chunky dinky humble lucky merry nimble
  plucky rosy shy snappy spry swift tidy witty zany brave calm clever
  curly dapper eager fancy glad golden misty velvet
)

NOUN=(
  otter crab robot mango disco panda mochi boba waffle pickle noodle
  bunny gecko llama koala sprout pebble acorn biscuit muffin teapot
  walrus pigeon donut comet clover fern maple olive peach plum radish
  turnip wombat yeti beetle cricket dolphin ferret hamster hedgehog
  kitten lemur marmot narwhal ocelot penguin quokka raccoon seal tadpole
  toucan urchin vole whale newt pudding sundae taco bagel crumpet scone
  crayon lantern
)

rand() {  # rand N → uniform-ish 0..N-1 from /dev/urandom
  echo $(( $(od -An -N2 -tu2 /dev/urandom | tr -d ' ') % $1 ))
}

TOKEN="${ADJ[$(rand ${#ADJ[@]})]}-${NOUN[$(rand ${#NOUN[@]})]}-${NOUN[$(rand ${#NOUN[@]})]}-$(printf '%02d' "$(rand 100)")"

if [ "${1:-}" = "--install" ]; then
  if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo for --install." >&2
    exit 1
  fi
  if [ -f "$ENV_FILE" ] && grep -q '^ADMIN_TOKEN=' "$ENV_FILE"; then
    sed -i "s/^ADMIN_TOKEN=.*/ADMIN_TOKEN=$TOKEN/" "$ENV_FILE"
  else
    echo "ADMIN_TOKEN=$TOKEN" >> "$ENV_FILE"
    chmod 600 "$ENV_FILE"
  fi
  systemctl restart cute-relay
  echo "New workshop token installed and relay restarted: $TOKEN"
  echo "Registry was wiped — existing devices must re-register via the flash tool."
else
  echo "$TOKEN"
fi
