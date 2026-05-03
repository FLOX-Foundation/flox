#!/usr/bin/env bash
# CI-equivalent check for the codegen pipeline. Run locally before pushing.
#   1. Regenerates the golden header from the spec.
#   2. Diffs against the committed golden — fails on drift.
#   3. Runs the signature check against the live flox_capi.h.
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

GOLDEN="$TOOL/golden/slice_capi.h"
SPEC="$REPO/include/flox/capi/flox_capi_spec.hpp"
LIVE="$REPO/include/flox/capi/flox_capi.h"

PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli emit-capi --spec "$SPEC" --out "$GOLDEN.tmp"

if ! diff -u "$GOLDEN" "$GOLDEN.tmp" >/dev/null; then
  echo "::error::golden header drift — run 'tools/codegen/scripts/regenerate.sh'"
  diff -u "$GOLDEN" "$GOLDEN.tmp" || true
  rm -f "$GOLDEN.tmp"
  exit 1
fi
rm -f "$GOLDEN.tmp"

PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli check \
  --expected "$LIVE" \
  --actual "$GOLDEN"

PYTHONPATH="$TOOL" "$PY" -m pytest "$TOOL/tests/" -q

echo "codegen check OK"
