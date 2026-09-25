#!/usr/bin/env python3
"""Mutation harness for the wake-set tests after the nets became settable.

The tests in tests/test_event_bus_wake_set.cpp push every safety net out to
ten seconds and then require each wake-up inside a round's spin budget, so a
wake-up that rides the net fails the round instead of failing a millisecond
bound a loaded runner is free to miss. That only proves something if a
broken wake path still makes the tests go red. Each mutation below removes
one wake, rebuilds the test, and requires it to fail.

Run from the repository root with a configured `build/` (FLOX_BUILD_TESTS=ON):

    python3 scripts/mutations/wake_net.py
"""

from __future__ import annotations

import glob
import hashlib
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build"
BUS = "include/flox/util/eventing/event_bus.h"
SET = "include/flox/util/eventing/wake_set.h"
TARGET = "test_event_bus_wake_set"

MUTATIONS = [
    (
        "a publish never wakes the set",
        BUS,
        "    if (_wakeSet != nullptr)\n    {\n      _wakeSet->wake();\n    }\n  }\n\n  // Wake whoever is parked. Called after a publish and on the way down.\n",
        "    if (false)\n    {\n      _wakeSet->wake();\n    }\n  }\n\n  // Wake whoever is parked. Called after a publish and on the way down.\n",
    ),
    (
        "a publish never wakes a parked consumer",
        BUS,
        "    if (_anyParked)\n    {\n      wakeParked();\n    }\n",
        "    if (false)\n    {\n      wakeParked();\n    }\n",
    ),
    (
        "the set's wake() wakes nobody",
        SET,
        "    if (_waiters.load(std::memory_order_seq_cst) == 0)\n    {\n      return;\n    }\n",
        "    if (true)\n    {\n      return;\n    }\n",
    ),
    (
        "the far net is ignored (equivalent: the mechanism, not the net, is under test)",
        SET,
        "  void setNetInterval(std::chrono::milliseconds interval) noexcept { _netInterval = interval; }\n",
        "  void setNetInterval(std::chrono::milliseconds) noexcept {}\n",
    ),
]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rebuild() -> tuple[bool, bool]:
    for o in glob.glob(str(BUILD / "**" / "CMakeFiles" / f"{TARGET}.dir" / "**" / "*.o"), recursive=True):
        os.remove(o)
    r = subprocess.run(
        ["cmake", "--build", str(BUILD), "-j4", "--target", TARGET],
        cwd=REPO, capture_output=True, text=True, timeout=3600,
    )
    return r.returncode == 0, "Building CXX" in r.stdout


def run() -> str:
    exe = BUILD / "tests" / TARGET
    if not exe.exists():
        return "NOBIN"
    try:
        r = subprocess.run([str(exe)], cwd=REPO, capture_output=True, text=True, timeout=600,
                           env=dict(os.environ, FLOX_REPO_ROOT=str(REPO)))
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    return "GREEN" if r.returncode == 0 else "RED"


def main() -> int:
    rows = []
    for name, rel, old, new in MUTATIONS:
        path = REPO / rel
        src = path.read_text()
        before = sha(path)
        if src.count(old) != 1:
            rows.append((name, f"ANCHOR x{src.count(old)}"))
            print(rows[-1], flush=True)
            continue
        path.write_text(src.replace(old, new))
        try:
            ok, built = rebuild()
            result = "NOCOMPILE" if not ok else run() + ("" if built else " (not rebuilt)")
        finally:
            path.write_text(src)
            assert sha(path) == before
        rows.append((name, result))
        print(f"{name:<80} {result}", flush=True)
    ok, _ = rebuild()
    control = run() if ok else "NOCOMPILE"
    rows.append(("control (restored)", control))
    print("\n=== TABLE ===")
    for name, result in rows:
        print(f"{name:<80} {result}")
    killed = sum(1 for n, r in rows[:-1] if r == "RED" and "equivalent" not in n)
    expected = sum(1 for n, *_ in MUTATIONS if "equivalent" not in n)
    print(f"\n{killed}/{expected} non-equivalent mutations red; control: {control}")
    return 0 if killed == expected and control == "GREEN" else 1


if __name__ == "__main__":
    sys.exit(main())
