#!/bin/bash
set -e
MODULE_ID="fouille"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."

docker build -t schwung-builder "$SCRIPT_DIR"
mkdir -p "$ROOT/dist/$MODULE_ID"

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$ROOT:/build" \
  schwung-builder \
  aarch64-linux-gnu-gcc \
    -O2 -shared -fPIC \
    -o "/build/dist/$MODULE_ID/dsp.so" \
    "/build/src/dsp/$MODULE_ID.c" \
    -lm -lpthread

cp "$ROOT/src/module.json" "$ROOT/dist/$MODULE_ID/"
tar -czf "$ROOT/dist/$MODULE_ID-module.tar.gz" -C "$ROOT/dist" "$MODULE_ID/"
echo "Built: dist/$MODULE_ID-module.tar.gz"
