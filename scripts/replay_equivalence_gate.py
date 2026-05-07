#!/usr/bin/env python3
"""Replay-equivalence CI gate (W2-T017).

Builds a deterministic tape, runs the frozen reference strategy
through it, and asserts the captured output is byte-equal to
``tests/replay-equivalence/expected_output.json``. This is the
guarantee behind flox's "deterministic backtest ↔ live replay"
positioning: if anything in the engine drifts the fill output,
this gate fails before merge.

Exits 0 on byte-equal match, 1 on divergence (with a per-key diff).

To regenerate the expected output after an intentional change, run
this script with ``--regen``: it will overwrite the JSON in place.
Reviewers should expect the diff in the PR; do not auto-regen on
unintentional changes.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
FIXTURES = REPO_ROOT / "tests" / "replay-equivalence"
STRATEGY_PATH = FIXTURES / "strategy.py"
EXPECTED_PATH = FIXTURES / "expected_output.json"


def _ensure_python_path() -> None:
    for cand in ("build/python", "build-py312/python"):
        p = REPO_ROOT / cand
        if p.is_dir() and str(p) not in sys.path:
            sys.path.insert(0, str(p))
            break
    sys.path.insert(0, str(FIXTURES))


def _diff_dicts(actual, expected, path: str = "") -> list[str]:
    diffs: list[str] = []
    if isinstance(actual, dict) and isinstance(expected, dict):
        keys = sorted(set(actual.keys()) | set(expected.keys()))
        for k in keys:
            a = actual.get(k, "<missing>")
            e = expected.get(k, "<missing>")
            sub = f"{path}.{k}" if path else k
            if isinstance(a, dict) and isinstance(e, dict):
                diffs.extend(_diff_dicts(a, e, sub))
            elif a != e:
                diffs.append(f"{sub}: actual={a!r} expected={e!r}")
    elif actual != expected:
        diffs.append(f"{path}: actual={actual!r} expected={expected!r}")
    return diffs


def run(*, regen: bool = False) -> int:
    _ensure_python_path()

    from flox_py.bundle import _run_strategy_against_tape  # type: ignore
    from build_tape import build_tape  # type: ignore

    work = Path(tempfile.mkdtemp(prefix="flox-replay-eq-"))
    try:
        tape_dir = work / "tape"
        build_tape(tape_dir)
        actual = _run_strategy_against_tape(STRATEGY_PATH, tape_dir)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if regen:
        EXPECTED_PATH.write_text(
            json.dumps(actual, indent=2, sort_keys=True) + "\n"
        )
        print(f"replay-equivalence gate: regenerated {EXPECTED_PATH}")
        return 0

    expected = json.loads(EXPECTED_PATH.read_text())
    diffs = _diff_dicts(actual, expected)
    if not diffs:
        print(
            f"replay-equivalence gate: OK "
            f"(trade_count={actual['trade_count']}, "
            f"fill_count={actual['fill_count']}, "
            f"total_filled={actual['total_filled_quantity']})"
        )
        return 0

    print("::error::replay-equivalence gate: DIVERGENCE", file=sys.stderr)
    for d in diffs:
        print(f"  {d}", file=sys.stderr)
    print(
        "\nIf this divergence is intentional, regenerate the expected "
        "output with:\n  python3 scripts/replay_equivalence_gate.py --regen",
        file=sys.stderr,
    )
    return 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--regen", action="store_true",
        help="Overwrite expected_output.json with the current run's output. "
             "Use only for deliberate engine changes; reviewers should see "
             "the diff in the PR.",
    )
    args = p.parse_args()
    return run(regen=args.regen)


if __name__ == "__main__":
    sys.exit(main())
