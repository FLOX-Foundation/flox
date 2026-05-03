#!/usr/bin/env bash
# CI-equivalent check for the codegen pipeline. Run locally before pushing.
#   1. Regenerates each golden artifact from the spec into a temp file.
#   2. Diffs each against the committed golden — fails on drift.
#   3. Runs the full-coverage signature check against the live flox_capi.h.
#   4. Runs unit tests.
#
# Exits non-zero on any failure. Idempotent.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$TOOL/../.." && pwd)"

PY="$TOOL/.venv/bin/python"
if [[ ! -x "$PY" ]]; then
  echo "venv missing; run tools/codegen/setup.sh first" >&2
  exit 2
fi

SPEC="$REPO/include/flox/capi/flox_capi_spec.hpp"
LIVE="$REPO/include/flox/capi/flox_capi.h"

verify_one() {
  local subcommand="$1" target="$2"
  local tmp="${target}.tmp"
  PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli "$subcommand" \
    --spec "$SPEC" --out "$tmp"
  if ! diff -u "$target" "$tmp" >/dev/null; then
    echo "::error::$(basename "$target") drift — run 'tools/codegen/scripts/regenerate.sh'"
    diff -u "$target" "$tmp" || true
    rm -f "$tmp"
    return 1
  fi
  rm -f "$tmp"
}

verify_one emit-capi  "$TOOL/golden/flox_capi.h"
verify_one emit-codon "$TOOL/golden/flox_capi.codon"
verify_one emit-llms  "$TOOL/golden/flox_capi.md"

PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli check \
  --expected "$LIVE" \
  --actual "$TOOL/golden/flox_capi.h" \
  --require-full-coverage

PYTHONPATH="$TOOL" "$PY" -m pytest "$TOOL/tests/" -q

echo "codegen check OK"
