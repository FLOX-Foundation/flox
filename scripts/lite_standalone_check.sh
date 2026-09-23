#!/usr/bin/env bash
# scripts/lite_standalone_check.sh — the rehearsal for T013 step 3.
#
# Before the venue core is ever cut into its own repository, this answers
# the question that matters: does a tree made of *only*
# scripts/mirror_paths.py's file list configure, build and pass the
# venue-lite profile's tests on its own, with the rest of this repository
# unreachable? It copies exactly that list into a scratch directory --
# nothing more, nothing walked or globbed beyond it -- and runs the same
# three commands docs/venue/build-profiles.md documents for the profile.
#
# A CMake configure warning about a missing subdirectory (python/, node/,
# connectors/, tests/, demo/, tools/, benchmarks/ outside venue/) means an
# `add_subdirectory` or `file(GLOB)` ran somewhere it should have been
# behind FLOX_VENUE_LITE's forced-off options -- this script treats that as
# a failure, not a warning to ignore, because the mirror tree does not have
# those directories at all.
#
# Usage:
#   scripts/lite_standalone_check.sh [scratch-dir]
#
# scratch-dir defaults to a fresh mktemp -d, removed on exit. Set KEEP=1 to
# leave it in place for inspection (its path is still printed either way).
# Set EXTRA_CMAKE_ARGS to pass extra flags to the configure step -- CI uses
# it for -DCMAKE_CXX_COMPILER=g++-14 and the ccache launcher, same as the
# venue-lite job. CCACHE_DIR (if set) is inherited as-is: it names an
# absolute path, so it works the same whether cmake runs from the repo or
# from the scratch directory.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

list_file="$(mktemp)"
configure_log="$(mktemp)"
scratch="${1:-$(mktemp -d "${TMPDIR:-/tmp}/flox-venue-lite-standalone.XXXXXX")}"
keep_scratch="${KEEP:-0}"

cleanup() {
  rm -f "$list_file" "$configure_log"
  if [ "$keep_scratch" != "1" ]; then
    rm -rf "$scratch"
  fi
}
trap cleanup EXIT

mkdir -p "$scratch"
echo "[lite-standalone] mirror tree: $scratch"

python3 scripts/mirror_paths.py > "$list_file"
n_paths=$(wc -l < "$list_file" | tr -d ' ')
echo "[lite-standalone] mirror_paths.py: $n_paths files"

# --files-from copies exactly the listed files (and the directories needed
# to hold them) and nothing else -- it does not walk the source tree, so a
# file that is not on the list is never even opened, let alone copied.
rsync -a --files-from="$list_file" "$repo_root/" "$scratch/"

copied=$(find "$scratch" -type f | wc -l | tr -d ' ')
if [ "$copied" -ne "$n_paths" ]; then
  echo "::error::mirror_paths.py listed $n_paths files, $copied landed in $scratch" >&2
  exit 1
fi

cd "$scratch"

extra_args=()
if [ -n "${EXTRA_CMAKE_ARGS:-}" ]; then
  read -ra extra_args <<< "$EXTRA_CMAKE_ARGS"
fi

echo "[lite-standalone] configuring"
if ! cmake --preset venue-lite "${extra_args[@]}" > "$configure_log" 2>&1; then
  cat "$configure_log" >&2
  echo "::error::configure failed in the standalone tree" >&2
  exit 1
fi
cat "$configure_log"

# CMake's own "Deprecation Warning" (the vendored simdjson FetchContent
# pulls one) is not this check's concern; a plain "CMake Warning" is --
# that is what a missing add_subdirectory target or a GLOB over an absent
# directory produces.
if grep -q "^CMake Warning" "$configure_log"; then
  echo "::error::the configure warned about something in the standalone tree -- see above" >&2
  exit 1
fi

if command -v nproc >/dev/null 2>&1; then
  njobs=$(nproc)
else
  njobs=$(sysctl -n hw.ncpu)
fi

echo "[lite-standalone] building"
cmake --build build-venue-lite -j"$njobs"

echo "[lite-standalone] testing"
ctest --preset venue-lite --output-on-failure

n_tests=$(ctest --test-dir build-venue-lite -N | sed -n 's/^Total Tests: //p')
echo "[lite-standalone] OK: $n_paths files, $n_tests tests"
