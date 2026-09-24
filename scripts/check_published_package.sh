#!/usr/bin/env bash
# Rehearse, by hand, the install-and-import gate that node-publish.yml and
# python-wheels.yml now run before publishing. Takes an already-built npm
# tarball (`npm pack` output) and/or an already-built Python wheel; this
# script does not build either -- it only proves that what would go to the
# registry actually installs and loads on this machine.
#
# This is the check that would have caught the flat prebuilds/flox_node.node
# bug: every npm publish since 0.7.1 shipped a package whose prebuilds/
# directory held one file that overwrote all three platform artifacts on
# download, while node/index.js requires prebuilds/<platform>-<arch>/..., so
# `require('@flox-foundation/flox')` failed on every install. Nothing in the
# old workflow ever ran that require -- publish went straight from
# download-artifact to `npm publish`.
#
# Usage:
#   scripts/check_published_package.sh --npm-tarball PATH [--wheel PATH]
#   scripts/check_published_package.sh --wheel PATH [--npm-tarball PATH]
#
# At least one of --npm-tarball / --wheel is required. Exit status is
# nonzero if any requested check fails.
set -euo pipefail

npm_tarball=""
wheel_path=""

usage() {
  echo "usage: $0 [--npm-tarball PATH] [--wheel PATH]" >&2
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --npm-tarball)
      npm_tarball="$2"
      shift 2
      ;;
    --wheel)
      wheel_path="$2"
      shift 2
      ;;
    -h|--help)
      usage
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage
      ;;
  esac
done

if [[ -z "$npm_tarball" && -z "$wheel_path" ]]; then
  usage
fi

fail=0

check_npm_tarball() {
  local tarball="$1"
  if [[ ! -f "$tarball" ]]; then
    echo "::error::npm tarball not found: $tarball" >&2
    return 1
  fi
  tarball="$(cd -- "$(dirname -- "$tarball")" && pwd)/$(basename -- "$tarball")"

  local work
  work=$(mktemp -d)
  trap 'rm -rf "$work"' RETURN

  echo "[npm-check] installing $(basename "$tarball") into a clean directory..."
  (
    cd "$work"
    npm init -y >/dev/null
    npm install "$tarball" >/dev/null
  )

  echo "[npm-check] requiring the package and calling into the native addon..."
  if ! node -e "
    const flox = require('$work/node_modules/@flox-foundation/flox');
    const reg = new flox.SymbolRegistry();
    const sym = reg.addSymbol('bybit', 'BTCUSDT', 0.01);
    if (sym === undefined || sym === null) {
      throw new Error('native addon call returned nothing');
    }
    console.log('[npm-check] OK: require() + SymbolRegistry.addSymbol() ->', sym);
  "; then
    echo "::error::npm package failed to load or call the native addon" >&2
    return 1
  fi
}

check_wheel() {
  local wheel="$1"
  if [[ ! -f "$wheel" ]]; then
    echo "::error::wheel not found: $wheel" >&2
    return 1
  fi
  wheel="$(cd -- "$(dirname -- "$wheel")" && pwd)/$(basename -- "$wheel")"

  local work
  work=$(mktemp -d)
  trap 'rm -rf "$work"' RETURN

  local py="${PYTHON:-python3}"
  echo "[wheel-check] installing $(basename "$wheel") into a clean venv..."
  "$py" -m venv "$work/venv"
  "$work/venv/bin/pip" install --quiet "$wheel"

  echo "[wheel-check] importing flox_py and calling into the native extension..."
  # Run from a directory with no python/flox_py/ nearby so cwd resolution
  # cannot shadow the installed wheel with the source tree.
  if ! (cd /tmp && "$work/venv/bin/python" -c "
import flox_py as flox
reg = flox.SymbolRegistry()
sid = reg.add_symbol('bybit', 'BTCUSDT', 0.01)
assert flox.SLIPPAGE_NONE == 0
assert sid is not None
print('[wheel-check] OK: import + SymbolRegistry.add_symbol() ->', sid)
"); then
    echo "::error::wheel failed to import or call the native extension" >&2
    return 1
  fi
}

if [[ -n "$npm_tarball" ]]; then
  if ! check_npm_tarball "$npm_tarball"; then
    fail=1
  fi
fi

if [[ -n "$wheel_path" ]]; then
  if ! check_wheel "$wheel_path"; then
    fail=1
  fi
fi

if [[ "$fail" -ne 0 ]]; then
  echo "::error::check_published_package.sh: one or more checks failed" >&2
  exit 1
fi

echo "check_published_package.sh: all requested checks passed"
