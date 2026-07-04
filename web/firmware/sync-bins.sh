#!/usr/bin/env bash
# Copy the three flashable binaries from the ESP-IDF build output into this
# folder, where the manifest (and GitHub Pages) expects them, and regenerate
# manifest.json with a content-hash query on each part. The hash busts the
# 4-hour browser/Cloudflare cache — without it, reflashing from the website
# can silently install a stale firmware.bin from cache.
# Run after every firmware build, then commit the bins + manifest.
set -euo pipefail
cd "$(dirname "$0")"
BUILD=../../firmware/build

cp "$BUILD/bootloader/bootloader.bin" .
cp "$BUILD/partition_table/partition-table.bin" .
cp "$BUILD/firmware.bin" .

V=$(md5 -q firmware.bin | cut -c1-8)
cat > manifest.json <<EOF
{
  "name": "cute.tech",
  "version": "$V",
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader.bin?v=$V", "offset": 0 },
        { "path": "partition-table.bin?v=$V", "offset": 32768 },
        { "path": "firmware.bin?v=$V", "offset": 65536 }
      ]
    }
  ]
}
EOF

ls -la ./*.bin
echo "manifest version: $V"
