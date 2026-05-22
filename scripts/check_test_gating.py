#!/usr/bin/env python3
"""Audit tests/*.cpp against their CMake gating blocks.

Reads `tests/CMakeLists.txt`, determines which test targets live under
which `if(FLAG)` block, then scans the corresponding source file for
includes from gated header prefixes. Fails the check when a test
includes a gated header but lives outside the matching block.

Examples of gated headers:
- `flox/backtest/...`   → requires FLOX_ENABLE_BACKTEST
- `flox/capi/...`       → requires FLOX_BUILD_CAPI

The check is conservative — only direct `#include` lines are inspected,
so transitive includes through engine headers are not chased. T020
caught one case (`test_live_queue_position`); this script automates the
audit for every future PR.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CMAKE_PATH = REPO_ROOT / "tests" / "CMakeLists.txt"
TESTS_DIR = REPO_ROOT / "tests"

# header prefix → required cmake flag
GATED_PREFIXES: dict[str, str] = {
    "flox/backtest/": "FLOX_ENABLE_BACKTEST",
    "flox/capi/": "FLOX_BUILD_CAPI",
}

# Hard-prereq implications expressed in CMakeLists.txt. When a flag
# is active, every flag it implies is also guaranteed active. Mirrors
# the message(FATAL_ERROR ...) guards at config time.
FLAG_IMPLIES: dict[str, set[str]] = {
    "FLOX_BUILD_CAPI": {"FLOX_ENABLE_BACKTEST"},
    "FLOX_BUILD_CODON": {"FLOX_BUILD_CAPI", "FLOX_ENABLE_BACKTEST"},
    "FLOX_BUILD_QUICKJS": {"FLOX_BUILD_CAPI", "FLOX_ENABLE_BACKTEST"},
}


def expand_flags(flags: list[str]) -> set[str]:
    """Resolve transitive flag implications."""
    out: set[str] = set(flags)
    changed = True
    while changed:
        changed = False
        for f in list(out):
            for implied in FLAG_IMPLIES.get(f, set()):
                if implied not in out:
                    out.add(implied)
                    changed = True
    return out

ADD_TEST_RE = re.compile(r"^\s*add_flox_test\(\s*([A-Za-z0-9_]+)\s*\)")
# Track every if(...) so the depth stays balanced. Non-flag forms
# (if(TARGET ...), if(NOT ...), etc.) get a sentinel that contributes
# no flag but still occupies a stack slot.
IF_ANY_RE = re.compile(r"^\s*if\s*\(")
IF_FLAG_RE = re.compile(r"^\s*if\(\s*([A-Za-z0-9_]+)\s*\)")
ENDIF_RE = re.compile(r"^\s*endif\s*\(")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

_SENTINEL = "<non-flag>"


def parse_cmake_gating(cmake_text: str) -> dict[str, list[str]]:
    """Return {test_target: [flags…]} based on nested if() blocks."""
    targets: dict[str, list[str]] = {}
    flag_stack: list[str] = []
    for raw in cmake_text.splitlines():
        line = raw.split("#", 1)[0]
        if IF_ANY_RE.match(line):
            m = IF_FLAG_RE.match(line)
            flag_stack.append(m.group(1) if m else _SENTINEL)
            continue
        if ENDIF_RE.match(line):
            if flag_stack:
                flag_stack.pop()
            continue
        if (m := ADD_TEST_RE.match(line)):
            targets[m.group(1)] = [f for f in flag_stack if f != _SENTINEL]
    return targets


def scan_includes(cpp_path: Path) -> set[str]:
    out: set[str] = set()
    try:
        with cpp_path.open("r", encoding="utf-8") as f:
            for line in f:
                m = INCLUDE_RE.match(line)
                if m:
                    out.add(m.group(1))
    except FileNotFoundError:
        pass
    return out


def required_flags_for_includes(includes: set[str]) -> set[str]:
    needed: set[str] = set()
    for inc in includes:
        for prefix, flag in GATED_PREFIXES.items():
            if inc.startswith(prefix):
                needed.add(flag)
    return needed


def main() -> int:
    if not CMAKE_PATH.exists():
        print(f"::error::{CMAKE_PATH} not found", file=sys.stderr)
        return 2

    targets = parse_cmake_gating(CMAKE_PATH.read_text(encoding="utf-8"))
    failures: list[str] = []
    coverage_lines: list[str] = []

    for target, current_flags in sorted(targets.items()):
        cpp = TESTS_DIR / f"{target}.cpp"
        includes = scan_includes(cpp)
        needed = required_flags_for_includes(includes)
        effective = expand_flags(current_flags)
        missing = needed - effective
        coverage_lines.append(
            f"  {target:<45} gating=[{', '.join(current_flags) or '-'}]  needs=[{', '.join(sorted(needed)) or '-'}]"
        )
        if missing:
            failures.append(
                f"::error::{target} ({cpp.name}) includes a gated header but "
                f"lives outside [{', '.join(sorted(missing))}] — move it inside "
                f"the matching if(...) block in {CMAKE_PATH.relative_to(REPO_ROOT)}."
            )

    print("Test gating coverage:")
    for line in coverage_lines:
        print(line)
    print()

    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        return 1
    print(f"OK — {len(targets)} test targets, gating consistent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
