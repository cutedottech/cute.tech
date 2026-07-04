#!/usr/bin/env bash
# Copy the three flashable binaries from the ESP-IDF build output into this
# folder, where the manifest (and GitHub Pages) expects them. Run after every
# firmware build, then commit the updated bins.
set -euo pipefail
cd "$(dirname "$0")"
BUILD=../../firmware/build

cp "$BUILD/bootloader/bootloader.bin" .
cp "$BUILD/partition_table/partition-table.bin" .
cp "$BUILD/firmware.bin" .
ls -la ./*.bin
