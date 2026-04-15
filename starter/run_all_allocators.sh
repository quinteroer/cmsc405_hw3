#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT_DIR/starter/bin"
mkdir -p "$BIN_DIR"

compile_and_run() {
  local label="$1"
  local src="$2"
  local bin="$3"

  echo "============================================================"
  echo "[$label] compiling: $src"
  gcc -std=c11 -Wall -Wextra -pedantic -pthread "$src" -o "$bin"

  echo "[$label] running: $bin"s
  "$bin"
  echo
}

echo "Running all allocator variants + wrapper harness"
echo "ROOT_DIR=$ROOT_DIR"
echo

compile_and_run "SLL First-Fit" \
  "$ROOT_DIR/starter/malloc_firstfit_sll.c" \
  "$BIN_DIR/firstfit_sll"

compile_and_run "DLL Best-Fit" \
  "$ROOT_DIR/starter/malloc_bestfit_dll.c" \
  "$BIN_DIR/bestfit_dll"

compile_and_run "DLL Worst-Fit" \
  "$ROOT_DIR/starter/malloc_worstfit_dll.c" \
  "$BIN_DIR/worstfit_dll"

# Wrapper harness for my_* allocator implementation in malloc.c
echo "============================================================"
echo "[Wrapper + malloc.c] compiling: starter/malloc.c + starter/wrapper.c"
gcc -std=c11 -Wall -Wextra -pedantic -pthread \
  "$ROOT_DIR/starter/malloc.c" \
  "$ROOT_DIR/starter/wrapper.c" \
  -o "$BIN_DIR/wrapper_runner"

echo "[Wrapper + malloc.c] running: $BIN_DIR/wrapper_runner"
"$BIN_DIR/wrapper_runner"

echo
echo "Done. All allocator runs completed."
