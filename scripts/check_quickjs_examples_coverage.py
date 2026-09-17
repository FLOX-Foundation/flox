#!/usr/bin/env python3
"""Audit quickjs/examples/*.js against what CI actually executes.

quickjs/examples/merged_tape_smoke.js and quickjs/examples/run_trace_smoke.js
shipped with their own commits (MergedTapeReader binding; the .floxrun
trace format) and were never added to the "Verify QuickJS runner" step of
.github/workflows/ci.yml. Nothing broke: a QuickJS example needs no build
registration, so there is no compile-time signal comparable to a missing
add_codon_executable(...) call. The example just sat in the tree, present
and never run, until this script started looking. This is T057/T058/T059's
class of defect (a file counted as coverage while nothing in CI executes
it) recurring for the fourth time, here on the QuickJS side, and the fix
mirrors scripts/check_codon_examples_coverage.py: the list of examples and
the list CI runs must be tied together by a check, not by memory.

Every `quickjs/examples/*.js` file must fall into exactly one of:

  1. Invoked directly by the "Verify QuickJS runner" step in
     `.github/workflows/ci.yml` (the only place flox_js_runner is pointed
     at an example in CI today).
  2. Named in `EXCEPTIONS` below with a reason. An explicit, reasoned
     exception is acceptable; silently shipping an example that nothing
     runs is the exact defect this script exists to catch.

A file in neither is an orphan, and this script fails the build.

Unlike the Codon examples, a QuickJS example is not compiled -- there is
no CMake registration step to audit separately. flox_js_runner interprets
the .js file directly, so "invoked in CI" is the only signal there is.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
EXAMPLES_DIR = REPO_ROOT / "quickjs" / "examples"
CI_YML = REPO_ROOT / ".github" / "workflows" / "ci.yml"

STEP_NAME = "Verify QuickJS runner"

# stem (no .js) -> human reason this file is deliberately not run in CI.
# Every key here must name a real file in quickjs/examples/ (checked
# below) -- a stale entry for a file that no longer exists, or that CI
# now runs, is itself flagged so the list can't silently drift either.
EXCEPTIONS: dict[str, str] = {}


def discover_examples() -> set[str]:
    return {p.stem for p in EXAMPLES_DIR.glob("*.js")}


def parse_runner_step(ci_text: str) -> set[str]:
    """Extract stems invoked by `./build/src/quickjs/flox_js_runner
    quickjs/examples/<stem>.js` lines inside the "Verify QuickJS runner"
    step."""
    step_m = re.search(
        rf"- name: {re.escape(STEP_NAME)}\n(.*?)(?=\n    - name:|\Z)",
        ci_text,
        re.DOTALL,
    )
    if not step_m:
        return set()
    return set(
        re.findall(
            r"flox_js_runner quickjs/examples/([\w.]+)\.js",
            step_m.group(1),
        )
    )


def main() -> int:
    examples = discover_examples()
    ci_text = CI_YML.read_text(encoding="utf-8")

    failures: list[str] = []

    executed = parse_runner_step(ci_text)
    if not executed:
        failures.append(
            f'Could not find the "{STEP_NAME}" step (or it invoked no '
            f"examples) in {CI_YML.relative_to(REPO_ROOT)} -- either the "
            f"step was renamed/removed, or every invocation line was "
            f"stripped. Either way, this script can no longer verify "
            f"QuickJS example coverage."
        )

    exceptions = set(EXCEPTIONS.keys())

    stale_exceptions = exceptions - examples
    for stem in sorted(stale_exceptions):
        failures.append(
            f"EXCEPTIONS['{stem}'] names a file that does not exist in "
            f"quickjs/examples/ -- remove the stale entry."
        )

    overlap = exceptions & executed
    for stem in sorted(overlap):
        failures.append(
            f"'{stem}' is both in EXCEPTIONS and actually executed in "
            f"CI -- the exception is stale, drop it from EXCEPTIONS in "
            f"{Path(__file__).name}."
        )

    stale_executed = executed - examples
    for stem in sorted(stale_executed):
        failures.append(
            f'{CI_YML.relative_to(REPO_ROOT)}\'s "{STEP_NAME}" step runs '
            f"quickjs/examples/{stem}.js, which does not exist -- fix or "
            f"remove that line."
        )

    orphans = examples - executed - exceptions
    for stem in sorted(orphans):
        failures.append(
            f"quickjs/examples/{stem}.js exists but is never executed in "
            f'CI, and is not in EXCEPTIONS -- either add a '
            f"`./build/src/quickjs/flox_js_runner "
            f'quickjs/examples/{stem}.js` line to the "{STEP_NAME}" step '
            f"of {CI_YML.relative_to(REPO_ROOT)}, or add a reasoned entry "
            f"to EXCEPTIONS in {Path(__file__).name}."
        )

    print(f"quickjs/examples/: {len(examples)} file(s)")
    print(f"  run via CI step:  {len(executed & examples)}")
    print(f"  exceptions:       {len(exceptions)}")
    for stem in sorted(exceptions):
        print(f"    - {stem}: {EXCEPTIONS[stem]}")

    if failures:
        print()
        for f in failures:
            print(f"::error::{f}", file=sys.stderr)
        return 1

    print()
    print(f"OK — all {len(examples)} quickjs/examples/*.js files are "
          f"either executed in CI or listed in EXCEPTIONS with a reason.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
