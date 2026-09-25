#!/usr/bin/env python3
"""Audit codon/examples/*.codon against what CI actually executes.

The same defect recurred three times: a file counted as
coverage (backtest dispatch, a smoke run, a graph example) while nothing
in CI ever executed it, because the "list of examples" and the "list CI
runs" were two lists maintained by hand in two different places with no
check tying them together. This script is that check.

Every `codon/examples/*.codon` file must fall into exactly one of:

  1. Listed in the `for bin in ...` loop of the "Run Codon smoke
     examples" step in `.github/workflows/ci.yml` (the fail-on-exit-code
     and fail-on-FAIL-line step -- see that step's own comment for why
     both signals matter).
  2. Invoked directly by the "Run Codon examples" step (the small
     hand-picked list of full runs).
  3. Executed by a script outside ci.yml that this file knows about
     (`EXTERNAL_EXECUTIONS` below) -- currently just the byte-identical
     parity gate.
  4. Named in `EXCEPTIONS` below with a reason. An explicit, reasoned
     exception is acceptable; silently building an example and never
     running it is the exact defect this script exists to catch.

A file in none of these is an orphan: it ships as compiled coverage
that nothing exercises, and this script fails the build.

This also runs a companion pass: every `codon/examples/*.codon` file
must have a matching `add_codon_executable(...)` call in
`codon/CMakeLists.txt`, or it is never even compiled -- catching the
one-level-earlier version of the same defect.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
EXAMPLES_DIR = REPO_ROOT / "codon" / "examples"
CODON_CMAKE = REPO_ROOT / "codon" / "CMakeLists.txt"
CI_YML = REPO_ROOT / ".github" / "workflows" / "ci.yml"

# stem (no .codon) -> human reason this file is deliberately not run in CI.
# Every key here must name a real file in codon/examples/ (checked below) --
# a stale entry for a file that no longer exists, or that CI now runs, is
# itself flagged so the list can't silently drift out of sync either.
EXCEPTIONS: dict[str, str] = {
    "binding_smoke": (
        "compiled only, on purpose -- see the file's own header. Codon "
        "realizes a function only where it is called, so this file's "
        "value is forcing realization of the binding surface at compile "
        "time; every statement in it already runs at module scope during "
        "the build, so a runtime invocation checks nothing a passing "
        "build didn't already check."
    ),
}

# stem -> (script that executes it, substring proving the invocation is
# still there). The script itself must also be invoked from ci.yml, or
# this reduces to "trust me" -- checked separately below.
EXTERNAL_EXECUTIONS: dict[str, tuple[str, str]] = {
    "byte_identical_fixture": (
        "scripts/floxrun_byte_identical_gate.py",
        "codon_byte_identical_fixture",
    ),
}


def discover_examples() -> set[str]:
    return {p.stem for p in EXAMPLES_DIR.glob("*.codon")}


def parse_cmake_targets(cmake_text: str) -> dict[str, str]:
    """Return {target_name: source_path_as_written} for every
    add_codon_executable(...) call, source path exactly as it appears
    (still holding the ${CMAKE_..._DIR} variable) so callers can tell
    codon/examples/ sources from docs/examples/ sources."""
    targets: dict[str, str] = {}
    for m in re.finditer(
        r"add_codon_executable\(\s*(\S+)\s+([^)]+?)\)",
        cmake_text,
        re.DOTALL,
    ):
        name, source = m.group(1).strip(), m.group(2).strip()
        source = re.sub(r"\s+", "", source)
        targets[name] = source
    return targets


def parse_smoke_loop(ci_text: str) -> set[str]:
    """Extract the `for bin in ... ; do` word list from the
    "Run Codon smoke examples" step."""
    step_m = re.search(
        r"- name: Run Codon smoke examples\n(.*?)(?=\n    - name:|\Z)",
        ci_text,
        re.DOTALL,
    )
    if not step_m:
        return set()
    loop_m = re.search(r"for bin in(.*?); do", step_m.group(1), re.DOTALL)
    if not loop_m:
        return set()
    words = loop_m.group(1).replace("\\", " ").split()
    return {w[len("codon_"):] for w in words if w.startswith("codon_")}


def parse_direct_run_step(ci_text: str) -> set[str]:
    """Extract stems actually invoked (not merely `test -f`'d) by the
    "Run Codon examples" step."""
    step_m = re.search(
        r"- name: Run Codon examples\n(.*?)(?=\n    - name:|\Z)",
        ci_text,
        re.DOTALL,
    )
    if not step_m:
        return set()
    stems = set()
    for m in re.finditer(r"\./build/codon/(codon_\w+)", step_m.group(1)):
        stems.add(m.group(1)[len("codon_"):])
    return stems


def check_external_executions(ci_text: str) -> list[str]:
    """Verify each EXTERNAL_EXECUTIONS entry still holds: the script
    still references the binary, and ci.yml still runs that script."""
    failures = []
    for stem, (script_rel, marker) in EXTERNAL_EXECUTIONS.items():
        script_path = REPO_ROOT / script_rel
        if not script_path.exists():
            failures.append(
                f"EXTERNAL_EXECUTIONS['{stem}'] points at {script_rel}, "
                f"which no longer exists."
            )
            continue
        if marker not in script_path.read_text(encoding="utf-8"):
            failures.append(
                f"{script_rel} no longer references {marker} -- "
                f"'{stem}' is not actually executed by it any more."
            )
        if script_rel not in ci_text:
            failures.append(
                f"{script_rel} is not invoked anywhere in "
                f"{CI_YML.relative_to(REPO_ROOT)} -- '{stem}' is not "
                f"actually executed in CI even though the script itself "
                f"still contains the call."
            )
    return failures


def main() -> int:
    examples = discover_examples()
    cmake_text = CODON_CMAKE.read_text(encoding="utf-8")
    ci_text = CI_YML.read_text(encoding="utf-8")

    failures: list[str] = []

    # Pass 1: every example file must be compiled.
    targets = parse_cmake_targets(cmake_text)
    built_stems: set[str] = set()
    for name, source in targets.items():
        m = re.search(r"examples/([\w.]+)\.codon\)?$", source)
        if m and "CMAKE_CURRENT_SOURCE_DIR" in source:
            built_stems.add(m.group(1))
    unbuilt = examples - built_stems
    for stem in sorted(unbuilt):
        failures.append(
            f"codon/examples/{stem}.codon has no add_codon_executable(...) "
            f"call in {CODON_CMAKE.relative_to(REPO_ROOT)} -- it never "
            f"compiles, let alone runs."
        )

    # Pass 2: every example file must run, or be a named exception.
    smoke_stems = parse_smoke_loop(ci_text)
    direct_stems = parse_direct_run_step(ci_text) & examples
    external_stems = set(EXTERNAL_EXECUTIONS.keys())
    failures.extend(check_external_executions(ci_text))

    executed = smoke_stems | direct_stems | external_stems
    exceptions = set(EXCEPTIONS.keys())

    stale_exceptions = exceptions - examples
    for stem in sorted(stale_exceptions):
        failures.append(
            f"EXCEPTIONS['{stem}'] names a file that does not exist in "
            f"codon/examples/ -- remove the stale entry."
        )

    overlap = exceptions & executed
    for stem in sorted(overlap):
        failures.append(
            f"'{stem}' is both in EXCEPTIONS and actually executed in "
            f"CI -- the exception is stale, drop it from EXCEPTIONS in "
            f"{Path(__file__).name}."
        )

    orphans = examples - executed - exceptions
    for stem in sorted(orphans):
        failures.append(
            f"codon/examples/{stem}.codon is built but never executed in "
            f"CI, and is not in EXCEPTIONS -- either add codon_{stem} to "
            f"the smoke loop in the \"Run Codon smoke examples\" step of "
            f"{CI_YML.relative_to(REPO_ROOT)}, or add a reasoned entry to "
            f"EXCEPTIONS in {Path(__file__).name}."
        )

    print(f"codon/examples/: {len(examples)} file(s)")
    print(f"  compiled:              {len(built_stems & examples)}")
    print(f"  run via smoke loop:    {len(smoke_stems & examples)}")
    print(f"  run via direct step:   {len(direct_stems)}")
    print(f"  run via external gate: {len(external_stems & examples)}")
    print(f"  exceptions:            {len(exceptions)}")
    for stem in sorted(exceptions):
        print(f"    - {stem}: {EXCEPTIONS[stem]}")

    if failures:
        print()
        for f in failures:
            print(f"::error::{f}", file=sys.stderr)
        return 1

    print()
    print(f"OK — all {len(examples)} codon/examples/*.codon files are "
          f"either executed in CI or listed in EXCEPTIONS with a reason.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
