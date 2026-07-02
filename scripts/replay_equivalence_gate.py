#!/usr/bin/env python3
"""Replay-equivalence CI gate (W2-T017).

Builds a deterministic tape and runs every frozen scenario under
``tests/replay-equivalence/scenarios/`` through it, comparing each
captured output field-by-field against the scenario's frozen
``expected_output.json``. Scenarios cover the plain market path plus
the conditional-order mechanics (stop-loss, take-profit, trailing
stop). This is the guarantee behind flox's "deterministic backtest ↔
live replay" positioning: if anything in the engine drifts the fill
output, this gate fails before merge.

Exits 0 when every scenario matches exactly, 1 on divergence (with a
per-field diff per scenario).

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
SCENARIOS_DIR = FIXTURES / "scenarios"


def _ensure_python_path() -> None:
    # Prefer a build directory whose compiled extension matches the
    # running interpreter's ABI tag; a mismatched .so is invisible to
    # the import system and flox_py degrades into its typing-stub
    # namespace package. Fall back to the first existing candidate so
    # the error surfaced on import stays the informative one.
    import sysconfig
    suffix = sysconfig.get_config_var("EXT_SUFFIX") or ""
    candidates = [
        REPO_ROOT / cand
        for cand in ("build/python", "build-py312/python")
        if (REPO_ROOT / cand).is_dir()
    ]
    chosen = next(
        (p for p in candidates
         if suffix and (p / "flox_py" / f"_flox_py{suffix}").exists()),
        candidates[0] if candidates else None,
    )
    if chosen is not None and str(chosen) not in sys.path:
        sys.path.insert(0, str(chosen))
    sys.path.insert(0, str(FIXTURES))


def _diff_dicts(actual, expected, path: str = "") -> list[str]:
    diffs: list[str] = []
    if isinstance(actual, dict) and isinstance(expected, dict):
        keys = sorted(set(actual.keys()) | set(expected.keys()))
        for k in keys:
            a = actual.get(k, "<missing>")
            e = expected.get(k, "<missing>")
            sub = f"{path}.{k}" if path else k
            diffs.extend(_diff_dicts(a, e, sub))
    elif isinstance(actual, list) and isinstance(expected, list):
        if len(actual) != len(expected):
            diffs.append(
                f"{path}: length actual={len(actual)} expected={len(expected)}"
            )
        for i, (a, e) in enumerate(zip(actual, expected)):
            diffs.extend(_diff_dicts(a, e, f"{path}[{i}]"))
    elif actual != expected:
        diffs.append(f"{path}: actual={actual!r} expected={expected!r}")
    return diffs


def run(*, regen: bool = False) -> int:
    _ensure_python_path()

    from flox_py.bundle import _run_strategy_against_tape  # type: ignore
    from build_tape import build_tape  # type: ignore

    scenarios = sorted(
        p for p in SCENARIOS_DIR.iterdir()
        if (p / "strategy.py").is_file()
    )
    if not scenarios:
        print("::error::replay-equivalence gate: no scenarios found",
              file=sys.stderr)
        return 1

    work = Path(tempfile.mkdtemp(prefix="flox-replay-eq-"))
    failed: list[str] = []
    try:
        tape_dir = work / "tape"
        build_tape(tape_dir)

        for scen in scenarios:
            actual = _run_strategy_against_tape(scen / "strategy.py", tape_dir)
            expected_path = scen / "expected_output.json"

            if regen:
                expected_path.write_text(
                    json.dumps(actual, indent=2, sort_keys=True) + "\n"
                )
                print(f"replay-equivalence gate: regenerated {expected_path}")
                continue

            expected = json.loads(expected_path.read_text())
            diffs = _diff_dicts(actual, expected)
            if not diffs:
                print(
                    f"replay-equivalence gate [{scen.name}]: OK "
                    f"(trade_count={actual['trade_count']}, "
                    f"fill_count={actual['fill_count']}, "
                    f"total_filled={actual['total_filled_quantity']})"
                )
                continue

            failed.append(scen.name)
            print(f"::error::replay-equivalence gate [{scen.name}]: DIVERGENCE",
                  file=sys.stderr)
            for d in diffs:
                print(f"  {d}", file=sys.stderr)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if regen or not failed:
        return 0

    print(
        f"\n{len(failed)} scenario(s) diverged: {', '.join(failed)}."
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
