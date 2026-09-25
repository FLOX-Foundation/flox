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

# The struct-layout tables are consumed where they live, not from golden/:
# the C header is included by tests/test_capi_event_layout.cpp and the Codon
# module is imported by codon/flox/dispatch.codon. check.sh regenerates both
# and diffs them in place, so an edit by hand is drift like any other.
PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli emit-layout-h     --spec "$SPEC" --out "$REPO/include/flox/capi/flox_capi_layout.h"
PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli emit-layout-codon --spec "$SPEC" --out "$REPO/codon/flox/layout.codon"
PYTHONPATH="$TOOL" "$PY" -m flox_codegen.cli abi-snapshot --header "$LIVE" --out "$REPO/.api/c-api.snapshot"

# The flox-mcp package bundles a copy of the ABI snapshot for offline use;
# CI fails the verify-docs-current job if the bundled file diverges from
# .api/c-api.snapshot. Run the sync immediately after regen so the two
# can never drift in a single PR.
"$PY" "$REPO/scripts/sync_mcp_data.py"

echo "regenerated golden/{flox_capi.h, flox_capi.codon, flox_capi.md} + flox_capi_layout.h + codon/flox/layout.codon + .api/c-api.snapshot + mcp data"
