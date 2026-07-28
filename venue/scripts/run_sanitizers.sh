#!/usr/bin/env bash
#
# flox venue -- sanitizer verification.
# Builds and runs the verification corpus under AddressSanitizer + Undefined-
# BehaviorSanitizer (memory safety, UB) and the concurrent paths under Thread-
# Sanitizer (data races). Run this before a release and in CI so regressions in
# memory/UB/race safety are caught. Any sanitizer diagnostic fails the run.
#
# Usage:  venue/scripts/run_sanitizers.sh
set -euo pipefail

cd "$(dirname "$0")/../.."  # repo root (venue/scripts -> flox)
ROOT="$(pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CXX="${CXX:-g++}"
STD="-std=c++23 -O1 -g"
INC="-Iinclude -Ivenue/include -isystem include -isystem /opt/homebrew/include"
GTEST="-L/opt/homebrew/lib -lgtest -lgtest_main"
SRC="venue/src/reject_reason.cpp"

# Halt on the first UBSan diagnostic (default is to print and continue).
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
export ASAN_OPTIONS="detect_leaks=0"  # this corpus is arena/stack-oriented, not leak-focused

fail=0

run_asan() {
  local name="$1"
  echo "== ASAN+UBSAN: $name =="
  if ! "$CXX" $STD -fsanitize=address,undefined $INC "venue/tests/$name.cpp" $SRC $GTEST -pthread -o "$OUT/$name" 2>"$OUT/$name.build"; then
    echo "  BUILD FAILED"; cat "$OUT/$name.build"; fail=1; return
  fi
  if ! "$OUT/$name" >"$OUT/$name.log" 2>&1; then
    echo "  RUN FAILED"; tail -20 "$OUT/$name.log"; fail=1; return
  fi
  tail -1 "$OUT/$name.log" | sed 's/^/  /'
}

run_tsan() {
  local name="$1"
  echo "== TSAN: $name =="
  if ! "$CXX" $STD -fsanitize=thread $INC "venue/tests/$name.cpp" $SRC $GTEST -pthread -o "$OUT/$name" 2>"$OUT/$name.build"; then
    echo "  BUILD FAILED"; cat "$OUT/$name.build"; fail=1; return
  fi
  if ! "$OUT/$name" >"$OUT/$name.log" 2>&1; then
    echo "  RUN FAILED (race or assertion)"; tail -20 "$OUT/$name.log"; fail=1; return
  fi
  tail -1 "$OUT/$name.log" | sed 's/^/  /'
}

# Memory / UB: parsers (hostile network input) + the heavy engine fuzzes/harnesses
# + the recovery/journal paths (spot + derivatives).
for t in test_venue_parser_fuzz test_venue_differential_fuzz test_venue_conservation_fuzz test_venue_perp_venue test_venue_multi_symbol_soak \
         test_venue_venue test_venue_perp_recovery test_venue_ledger test_venue_ledger_primitive test_venue_cross_margin; do
  run_asan "$t"
done

# Data races: the single-writer sequenced core and the multi-threaded gateways.
for t in test_venue_sequenced test_venue_network; do
  run_tsan "$t"
done

echo
if [ "$fail" -eq 0 ]; then
  echo "ALL SANITIZER RUNS CLEAN"
else
  echo "SANITIZER FAILURES DETECTED"
fi
exit "$fail"
