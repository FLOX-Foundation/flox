#!/usr/bin/env bash
# Run a command, then read its output before calling the run green.
#
# The default `ctest --output-on-failure` shows the transcript of the tests
# ctest decided had failed. A sanitizer report printed by a test whose
# assertions all passed never reaches the screen, and whether it reaches the
# exit code at all depends on sanitizer options that differ per job. This
# wrapper keeps the command's own exit code and adds one thing: it greps the
# transcript for sanitizer reports and fails on them.
#
# Usage:
#   scripts/run-with-sanitizer-scan.sh ctest --output-on-failure --test-dir build
#   scripts/run-with-sanitizer-scan.sh ./build/demo/flox_demo
#
# For a ctest command the wrapper also scans ctest's own per-test transcript
# (build/Testing/Temporary/LastTest.log), which records every test's output
# whatever its verdict -- that file is where a passing test's report hides.
# The build directory is taken from --test-dir; set FLOX_CTEST_BUILD_DIR to
# override.
#
# On any failure (the command itself, or the scan) this wrapper dumps the
# transcript(s) it captured before exiting. Live streaming through `tee`
# usually shows the same bytes already, but that is not something to lean
# on: a runner that renders or folds output differently, or -- for ctest --
# LastTest.log, which is never streamed live at all, only scanned. A wrapper
# whose whole job is "make sure nobody has to guess what happened" should
# not itself be the reason a failure reads as a bare exit code.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PYTHON="${PYTHON:-python3}"

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

# Explicit template: `mktemp -t NAME` means different things on GNU and BSD.
capture="$(mktemp "${TMPDIR:-/tmp}/flox-run-scan.XXXXXX")"
trap 'rm -f "$capture"' EXIT

build_dir="${FLOX_CTEST_BUILD_DIR:-}"
is_ctest=0
case "$1" in
    *ctest*) is_ctest=1 ;;
esac
if [ -z "$build_dir" ]; then
    prev=""
    for arg in "$@"; do
        if [ "$prev" = "--test-dir" ]; then
            build_dir="$arg"
            break
        fi
        case "$arg" in
            --test-dir=*) build_dir="${arg#--test-dir=}"; break ;;
        esac
        prev="$arg"
    done
fi

# Stream to the terminal and keep a copy. `set -o pipefail` plus
# PIPESTATUS gives us the command's status, not tee's.
"$@" 2>&1 | tee "$capture"
status="${PIPESTATUS[0]}"

logs=("$capture")
ctest_log=""
if [ "$is_ctest" -eq 1 ]; then
    if [ -z "$build_dir" ]; then
        echo "::error::run-with-sanitizer-scan: ctest command without --test-dir; set FLOX_CTEST_BUILD_DIR" >&2
        exit 2
    fi
    ctest_log="$build_dir/Testing/Temporary/LastTest.log"
    if [ ! -f "$ctest_log" ]; then
        # No transcript means the run cannot be shown to be clean, and that
        # must not read the same as being clean.
        echo "::error::run-with-sanitizer-scan: $ctest_log is missing, so this run cannot be checked for sanitizer reports" >&2
        exit 2
    fi
    logs+=("$ctest_log")
fi

echo "--- scanning the transcript for sanitizer reports"
"$PYTHON" "$ROOT_DIR/scripts/scan_sanitizer_reports.py" "${logs[@]}"
scan_status=$?

dump_transcripts() {
    # Only called on failure -- a green run does not need its transcript
    # printed a second time, and doing it unconditionally would bloat every
    # log in the matrix for no reason.
    echo "::group::run-with-sanitizer-scan: captured transcript (command exited $status)"
    cat "$capture"
    echo "::endgroup::"
    if [ -n "$ctest_log" ]; then
        echo "::group::run-with-sanitizer-scan: $ctest_log"
        cat "$ctest_log"
        echo "::endgroup::"
    fi
}

if [ "$status" -ne 0 ]; then
    dump_transcripts
    exit "$status"
fi
if [ "$scan_status" -ne 0 ]; then
    echo "::error::the command exited 0 but its output carries a sanitizer report" >&2
    dump_transcripts
    exit "$scan_status"
fi
exit 0
