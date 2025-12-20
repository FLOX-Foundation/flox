#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"

cd "$ROOT_DIR"

echo "=== Format check ==="
./scripts/check-format.sh

echo "=== Clean build ==="
rm -rf "$BUILD_DIR"
cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFLOX_ENABLE_TESTS=ON \
    -DFLOX_ENABLE_BENCHMARKS=ON \
    -DFLOX_ENABLE_DEMO=ON \
    -DFLOX_ENABLE_LZ4=ON \
    -DFLOX_ENABLE_BACKTEST=ON

echo "=== Build ==="
cmake --build "$BUILD_DIR" -j$(nproc)

echo "=== Tests ==="
ctest --output-on-failure --test-dir "$BUILD_DIR"

echo "=== Demo ==="
"$BUILD_DIR/demo/flox_demo"

echo "=== Benchmarks ==="
./scripts/run-benchmarks.sh "$BUILD_DIR/benchmarks"

echo "=== All CI checks passed ==="
