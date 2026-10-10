#!/usr/bin/env bash
# Build the deprecated flox-py and flox-mcp compatibility packages, an sdist
# and a wheel each, into <outdir>. Nothing is uploaded; publishing them is a
# separate, deliberate step.
#
# Usage: scripts/build_compat_shims.sh <outdir>
#
# The shims pin the exact lrvx / lrvx-mcp version this tree builds, and
# the script refuses to build when a pin has drifted from the version in
# pyproject.toml / mcp/pyproject.toml.
#
# `python -m build` runs from a scratch directory: started from the
# repository root, the build/ tree there shadows the `build` module.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <outdir>" >&2
  exit 2
fi

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# Absolute, so a relative $PYTHON still resolves after the cd below.
py="$("${PYTHON:-python3}" -c 'import sys; print(sys.executable)')"
mkdir -p "$1"
outdir="$(cd -- "$1" && pwd)"

version_of() { sed -n 's/^version = "\(.*\)"$/\1/p' "$1" | head -1; }

check_pin() {
  local pyproject="$1" dist="$2" version="$3"
  if ! grep -q "\"$dist==$version\"" "$pyproject"; then
    echo "::error::$pyproject must depend on $dist==$version, the version this tree builds" >&2
    exit 1
  fi
}

check_pin "$repo_root/compat/flox-py/pyproject.toml" lrvx "$(version_of "$repo_root/pyproject.toml")"
check_pin "$repo_root/compat/flox-mcp/pyproject.toml" lrvx-mcp "$(version_of "$repo_root/mcp/pyproject.toml")"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

if ! "$py" -c "import build" 2>/dev/null; then
  "$py" -m venv "$work/venv"
  "$work/venv/bin/python" -m pip install --quiet build
  py="$work/venv/bin/python"
fi

for shim in flox-py flox-mcp; do
  "$py" -m build --outdir "$outdir" "$repo_root/compat/$shim"
done

ls -1 "$outdir"
