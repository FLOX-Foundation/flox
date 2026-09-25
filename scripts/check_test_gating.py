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
so transitive includes through engine headers are not chased. A past
manual audit caught one case (`test_live_queue_position`); this script automates the
audit for every future PR.

This also runs the reverse pass: every `tests/*.cpp` file must be named
by some `add_flox_test(...)` call or a manual `add_executable(...)` in
`tests/CMakeLists.txt`, or it is never compiled and never run — ctest
can only run what CMake was told to configure. A file left off the list
(add_flox_test) or off the manual add_executable() source list fails
the check. This only covers tests/ itself; venue/tests/ and
connectors/tests/ use file(GLOB ...) instead of a name list, so a file
dropped there is auto-discovered rather than silently orphaned, and
falls outside this script's scope (see check_suite_discovery.py).
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
    "flox/ml/": "FLOX_ENABLE_ONNX",
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
# Manual registration path (bypasses add_flox_test entirely): e.g.
#   add_executable(test_capi_logger test_capi_logger.cpp)
# The function's own template line, `add_executable(${name} ${name}.cpp)`,
# does not match this — `${name}` contains characters outside
# [A-Za-z0-9_.], so the literal-source-list form below only matches real
# manual registrations.
ADD_EXECUTABLE_RE = re.compile(
    r"^\s*add_executable\(\s*[A-Za-z0-9_]+\s+((?:[A-Za-z0-9_./]+\.cpp\s*)+)\)"
)
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


def find_unregistered(cmake_text: str) -> list[str]:
    """Return tests/*.cpp basenames that no add_flox_test(...) or manual
    add_executable(...) call in tests/CMakeLists.txt compiles.

    ctest only runs what CMake was told to configure; a source file that
    is not an argument to either registration form is never built and
    never run, and nothing else in the C++ toolchain would ever say so.
    """
    registered: set[str] = set()
    for raw in cmake_text.splitlines():
        line = raw.split("#", 1)[0]
        if (m := ADD_TEST_RE.match(line)):
            registered.add(f"{m.group(1)}.cpp")
            continue
        if (m := ADD_EXECUTABLE_RE.match(line)):
            for src in m.group(1).split():
                registered.add(Path(src).name)

    return sorted(
        p.name for p in TESTS_DIR.glob("*.cpp") if p.name not in registered
    )


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

    cmake_text = CMAKE_PATH.read_text(encoding="utf-8")
    targets = parse_cmake_gating(cmake_text)
    failures: list[str] = []
    coverage_lines: list[str] = []

    unregistered = find_unregistered(cmake_text)
    for name in unregistered:
        failures.append(
            f"::error::tests/{name} exists but is not named by any "
            f"add_flox_test(...) or add_executable(...) call in "
            f"{CMAKE_PATH.relative_to(REPO_ROOT)} — it is never compiled "
            f"and never runs."
        )

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
    print(
        f"Registration completeness: {len(list(TESTS_DIR.glob('*.cpp')))} "
        f"tests/*.cpp file(s), {len(unregistered)} unregistered."
    )

    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        return 1
    print(f"OK — {len(targets)} test targets, gating consistent, "
          f"every tests/*.cpp file is registered.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
