#!/usr/bin/env bash
# Regenerate the golden codegen output from the spec. Run after editing
# include/lrvx/capi/lrvx_capi_spec.hpp.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$TOOL/../.." && pwd)"

PY="$TOOL/.venv/bin/python"
if [[ ! -x "$PY" ]]; then
  bash "$TOOL/setup.sh"
fi

SPEC="$REPO/include/lrvx/capi/lrvx_capi_spec.hpp"
LIVE="$REPO/include/lrvx/capi/lrvx_capi.h"

PYTHONPATH="$TOOL" "$PY" -m lrvx_codegen.cli emit-capi    --spec "$SPEC"   --out "$TOOL/golden/lrvx_capi.h"
PYTHONPATH="$TOOL" "$PY" -m lrvx_codegen.cli emit-codon   --spec "$SPEC"   --out "$TOOL/golden/lrvx_capi.codon"
PYTHONPATH="$TOOL" "$PY" -m lrvx_codegen.cli emit-llms    --spec "$SPEC"   --out "$TOOL/golden/lrvx_capi.md"

# The struct-layout tables are consumed where they live, not from golden/:
# the C header is included by tests/test_capi_event_layout.cpp and the Codon
# module is imported by codon/lrvx/dispatch.codon. check.sh regenerates both
# and diffs them in place, so an edit by hand is drift like any other.
PYTHONPATH="$TOOL" "$PY" -m lrvx_codegen.cli emit-layout-h     --spec "$SPEC" --out "$REPO/include/lrvx/capi/lrvx_capi_layout.h"
PYTHONPATH="$TOOL" "$PY" -m lrvx_codegen.cli emit-layout-codon --spec "$SPEC" --out "$REPO/codon/lrvx/layout.codon"
PYTHONPATH="$TOOL" "$PY" -m lrvx_codegen.cli abi-snapshot --header "$LIVE" --out "$REPO/.api/c-api.snapshot"

# The lrvx-mcp package bundles a copy of the ABI snapshot for offline use;
# CI fails the verify-docs-current job if the bundled file diverges from
# .api/c-api.snapshot. Run the sync immediately after regen so the two
# can never drift in a single PR.
"$PY" "$REPO/scripts/sync_mcp_data.py"

echo "regenerated golden/{lrvx_capi.h, lrvx_capi.codon, lrvx_capi.md} + lrvx_capi_layout.h + codon/lrvx/layout.codon + .api/c-api.snapshot + mcp data"
