#!/usr/bin/env bash
# Bootstrap the codegen virtualenv. Idempotent.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="$HERE/.venv"

if [[ ! -d "$VENV" ]]; then
  echo "creating venv at $VENV"
  python3 -m venv "$VENV"
fi

"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet -r "$HERE/requirements.txt"

echo "ready: $VENV"
echo "use:   $VENV/bin/python -m flox_codegen.cli --help"
