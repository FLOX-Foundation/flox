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

# Same set as the verify-docs-current job in .github/workflows/ci.yml, so a
# green local run means green docs in CI. See docs/contributors/doc-gates.md.
echo "=== Documentation gates ==="
PY="${PYTHON:-python3}"
"$PY" scripts/gen_indicator_docs.py
git diff --exit-code -- docs/ >/dev/null || {
  echo "error: docs/ out of sync with include/flox/indicator/registry.def" >&2
  echo "Run: $PY scripts/gen_indicator_docs.py" >&2
  exit 1
}
"$PY" scripts/gen_llms_txt.py --check
"$PY" scripts/gen_api_index.py --check
"$PY" scripts/check_dts_exports.py
"$PY" scripts/check_dts_class_members.py
"$PY" scripts/check_binding_parity.py
"$PY" scripts/check_error_codes.py
"$PY" scripts/check_test_gating.py
"$PY" scripts/check_suite_discovery.py
"$PY" scripts/check_binding_smoke.py
"$PY" scripts/check_doc_snippets.py --min-includes 24
"$PY" scripts/check_doc_symbols.py
"$PY" scripts/check_doc_nav.py
"$PY" scripts/check_doc_links.py
"$PY" scripts/check_doc_conventions.py
"$PY" scripts/check_doc_examples.py
"$PY" scripts/sync_mcp_data.py --check

echo "=== All CI checks passed ==="
