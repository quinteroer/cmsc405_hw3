#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_BIN="$ROOT_DIR/starter/bin/allocator_runner"

# Build the allocator + wrapper harness.
gcc -std=c11 -Wall -Wextra -pedantic \
  "$ROOT_DIR/starter/malloc.c" \
  "$ROOT_DIR/starter/wrapper.c" \
  -o "$OUT_BIN"

# Execute the harness.
"$OUT_BIN" "$@"
