#!/usr/bin/env bash
# Regenerate the golden codegen output from the spec. Run after editing
# include/flox/capi/flox_capi_spec.hpp.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$TOOL/../.." && pwd)"

PY="$TOOL/.venv/bin/python"
if [[ ! -x "$PY" ]]; then
  bash "$TOOL/setup.sh"
fi

GOLDEN="$TOOL/golden/flox_capi.h"
SPEC="$REPO/include/flox/capi/flox_capi_spec.hpp"

PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli emit-capi --spec "$SPEC" --out "$GOLDEN"
echo "regenerated $GOLDEN"
