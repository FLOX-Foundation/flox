#!/usr/bin/env python3
"""Mutation harness for the SymbolStateMap and PositionTracker fixes.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each fix one at a
time, in the source, and checks that the test written for it goes red -- and
that an unmutated tree goes green before and after.

Every run is honest about the build: the mutated file's hash is printed before
and after, the target's object files are deleted so nothing can be served from
cache, the rebuild output has to contain "Building CXX" or the run is refused,
and the test binary runs under a timeout.

Usage:

    python3 scripts/mutations/symbol_state_map.py            # control, all mutations, control
    python3 scripts/mutations/symbol_state_map.py --list
    python3 scripts/mutations/symbol_state_map.py --only overflow-vector-instead-of-deque

The build directory is expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 120

SYMBOL_STATE_MAP = "include/flox/strategy/symbol_state_map.h"
POSITION_TRACKER = "include/flox/position/position_tracker.h"

TARGET = "test_multi_symbol_strategy"


@dataclass
class Mutation:
    name: str
    why: str
    file: str
    old: str
    new: str
    test: str
    target: str = TARGET
    occurrence: int = 1
    expected_occurrences: int = 1


MUTATIONS: list[Mutation] = [
    Mutation(
        name="overflow-vector-instead-of-deque",
        why="restores std::vector for the overflow tier; growing past capacity "
            "reallocates and moves every existing element, invalidating every "
            "reference operator[]/tryGet/an iterator already handed out",
        file=SYMBOL_STATE_MAP,
        old="std::deque<std::pair<SymbolId, T>> data;",
        new="std::vector<std::pair<SymbolId, T>> data;",
        test="SymbolStateMapTest.OverflowReferencesStableAcrossGrowth"
             ":SymbolStateMapTest.OverflowReferenceUsableAfterLaterInsertions",
    ),
    Mutation(
        name="clear-skips-flat-reset-for-non-movable-state",
        why="restores the original clear(): flat is only reassigned for a "
            "move-constructible State, so a non-movable State's data survives "
            "clear() under a freshly-cleared initialized flag",
        file=SYMBOL_STATE_MAP,
        old="""    // Destroy-and-reconstruct in place rather than `flat = {}`: assigning a
    // freshly-defaulted array requires State to be move- (or copy-)
    // assignable, which the non-movable case (State holding atomics) is
    // not, so that assignment used to be skipped entirely for it -- leaving
    // the old data behind under freshly-cleared `initialized` flags, i.e.
    // the next `operator[]` on that symbol handed back the previous run's
    // state as if it were new. Placement construction only needs State to
    // be default-constructible, which every State here already is (the
    // `Table` and the overflow scratch slot both default-construct it).
    for (State& state : _table->flat)
    {
      std::destroy_at(&state);
      std::construct_at(&state);
    }
    _table->initialized = {};
    _overflowStorage.clear();
    if constexpr (!std::is_move_constructible_v<State>)
    {
      std::destroy_at(&_overflowScratch);
      std::construct_at(&_overflowScratch);
    }""",
        new="""    if constexpr (std::is_move_constructible_v<State>)
    {
      _table->flat = {};
    }
    _table->initialized = {};
    _overflowStorage.clear();""",
        test="SymbolStateMapTest.ClearResetsNonMovableState",
    ),
    Mutation(
        name="clear-skips-overflow-storage-clear",
        why="drops `_overflowStorage.clear()` from clear(); the flat table "
            "and initialized flags reset, but a movable State's overflow "
            "entries survive, so size()/forEach() and a subsequent lookup "
            "still see the pre-clear() overflow data",
        file=SYMBOL_STATE_MAP,
        old="""    _table->initialized = {};
    _overflowStorage.clear();
    if constexpr (!std::is_move_constructible_v<State>)""",
        new="""    _table->initialized = {};
    if constexpr (!std::is_move_constructible_v<State>)""",
        test="SymbolStateMapTest.ClearEmptiesOverflowStorage",
    ),
    Mutation(
        name="clear-skips-overflow-scratch-reset",
        why="drops the destroy+construct of _overflowScratch from clear(); "
            "for a non-movable State the shared out-of-range scratch slot "
            "keeps its old value, so a later out-of-range read (on any "
            "symbol, not just the one that wrote it) reads stale data "
            "through the shared slot",
        file=SYMBOL_STATE_MAP,
        old="""    if constexpr (!std::is_move_constructible_v<State>)
    {
      std::destroy_at(&_overflowScratch);
      std::construct_at(&_overflowScratch);
    }
  }""",
        new="""    if constexpr (!std::is_move_constructible_v<State>)
    {
      (void)0;
    }
  }""",
        test="SymbolStateMapTest.ClearResetsOverflowScratchForNonMovableState",
    ),
    Mutation(
        name="position-tracker-states-mutable",
        why="restores `mutable` on _states; a mutable member is never const "
            "regardless of the enclosing method, so PositionTracker's const "
            "query methods pick SymbolStateMap's non-const operator[], which "
            "marks the symbol initialized (and for symbol >= 256, allocates "
            "an overflow entry) on a plain read",
        file=POSITION_TRACKER,
        old="  SymbolStateMap<PositionState> _states;",
        new="  mutable SymbolStateMap<PositionState> _states;",
        test="PositionTrackerTest.ConstQueriesDoNotMarkSymbolInitialized"
             ":PositionTrackerTest.ConstQueriesDoNotAllocateOverflowEntry",
    ),
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_occurrence(text: str, old: str, new: str, occurrence: int, expected: int) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(
            f"mutation anchor found {count} time(s), expected {expected}:\n  {old!r}"
        )
    start = -1
    for _ in range(occurrence):
        start = text.index(old, start + 1)
    return text[:start] + new + text[start + len(old):]


def object_files(target: str) -> list[Path]:
    """The target's own object files, located the way `find` would."""
    out = subprocess.run(
        ["find", str(BUILD), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


def rebuild(target: str) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise SystemExit(f"rebuild of {target} failed:\n{output[-4000:]}")
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} compiled nothing -- the result would have been "
            f"a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    return output


def run_test(target: str, gtest_filter: str | None) -> tuple[int, str]:
    binary = BUILD / "tests" / target
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def control(targets: list[str]) -> bool:
    ok = True
    for target in targets:
        # Deleted first so the control binary is compiled from the source as it
        # stands right now, not served from whatever the last run left behind.
        for obj in object_files(target):
            obj.unlink()
        rebuild(target)
        code, output = run_test(target, None)
        state = "green" if code == 0 else "RED"
        summary = next((line for line in output.splitlines() if line.startswith("[==========] ")
                        and " ran." in line), "")
        print(f"  control {target:<32} {state}   {summary.strip()}")
        ok = ok and code == 0
    return ok


def run_mutation(m: Mutation) -> bool:
    path = REPO / m.file
    original = path.read_text()
    before = sha256(path)
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    print(f"  file    {m.file}")
    print(f"  sha256  before  {before}")

    mutated = replace_occurrence(original, m.old, m.new, m.occurrence, m.expected_occurrences)
    if mutated == original:
        raise SystemExit("mutation changed nothing")
    path.write_text(mutated)
    print(f"  sha256  mutated {sha256(path)}")

    try:
        removed = object_files(m.target)
        for obj in removed:
            obj.unlink()
        print(f"  removed {len(removed)} object file(s) for {m.target}")

        output = rebuild(m.target)
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {m.target}: {compiled} 'Building CXX' line(s)")

        code, test_output = run_test(m.target, m.test)
        killed = code != 0
        print(f"  {m.target} --gtest_filter={m.test} -> exit {code} "
              f"({'RED, mutation killed' if killed else 'GREEN, MUTATION SURVIVED'})")
        if not killed:
            print("  ----- surviving mutation, test output -----")
            print("\n".join(test_output.splitlines()[-25:]))
    finally:
        path.write_text(original)
        after = sha256(path)
        print(f"  sha256  after   {after}")
        if after != before:
            raise SystemExit("restore failed: the file does not hash back to its original")
        for obj in object_files(m.target):
            obj.unlink()

    return killed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<44} {m.target:<30} {m.test}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD} is not configured; run\n"
            "  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON"
        )

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    targets = sorted({m.target for m in selected})

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    for m, killed in results:
        print(f"  {'RED  ' if killed else 'ALIVE'}  {m.name:<44} {m.test}")
    survived = [m.name for m, killed in results if not killed]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
