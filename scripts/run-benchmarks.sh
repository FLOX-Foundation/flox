#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR=${1:-build}

echo "[*] Looking for benchmarks in $BUILD_DIR"

# Use -perm to check executable bit (cross-platform, works on macOS and Linux)
# -perm +111 is BSD syntax, -perm /111 is GNU syntax
# Try BSD first (macOS), fall back to GNU (Linux)
if find "$BUILD_DIR" -maxdepth 1 -type f -perm +111 -name '*benchmark*' >/dev/null 2>&1; then
  BENCHES=$(find "$BUILD_DIR" -maxdepth 1 -type f -perm +111 -name '*benchmark*')
else
  BENCHES=$(find "$BUILD_DIR" -maxdepth 1 -type f -perm /111 -name '*benchmark*')
fi

if [[ -z "$BENCHES" ]]; then
  echo "[!] No benchmark executables found."
  exit 1
fi

FAILED=0

for bench in $BENCHES; do
  echo "[+] Running benchmark: $bench"
  if ! "$bench"; then
    echo "[!] Benchmark $bench failed."
    FAILED=1
  fi
done

if [[ "$FAILED" -eq 1 ]]; then
  echo "[!] Some benchmarks failed."
  exit 1
fi

echo "[✓] All benchmarks passed."
