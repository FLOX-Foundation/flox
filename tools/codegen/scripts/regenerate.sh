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

SPEC="$REPO/include/flox/capi/flox_capi_spec.hpp"
LIVE="$REPO/include/flox/capi/flox_capi.h"

PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli emit-capi    --spec "$SPEC"   --out "$TOOL/golden/flox_capi.h"
PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli emit-codon   --spec "$SPEC"   --out "$TOOL/golden/flox_capi.codon"
PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli emit-llms    --spec "$SPEC"   --out "$TOOL/golden/flox_capi.md"
PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli abi-snapshot --header "$LIVE" --out "$REPO/.api/c-api.snapshot"

# The flox-mcp package bundles a copy of the ABI snapshot for offline use;
# CI fails the verify-docs-current job if the bundled file diverges from
# .api/c-api.snapshot. Run the sync immediately after regen so the two
# can never drift in a single PR.
"$PY" "$REPO/scripts/sync_mcp_data.py"

echo "regenerated golden/{flox_capi.h, flox_capi.codon, flox_capi.md} + .api/c-api.snapshot + mcp data"
