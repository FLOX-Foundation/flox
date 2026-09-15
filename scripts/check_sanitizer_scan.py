#!/usr/bin/env python3
"""Keep the sanitizer transcript scan wired into the jobs that need it.

A sanitizer report printed by a test whose assertions all passed does not
reach `ctest --output-on-failure`, and whether it reaches the exit code
depends on per-sanitizer options that differ between jobs.
scripts/run-with-sanitizer-scan.sh closes that by reading the transcript, but
only for the steps it actually wraps. A job added later, or a step someone
un-wraps while debugging, silently reopens the hole -- and the symptom is a
green run, which is the one symptom nobody investigates.

So this asserts the wiring over the workflow files rather than trusting
memory: in any job that compiles with `-fsanitize=`, every step that runs the
test suite or a built binary goes through the wrapper.

Windows jobs are out of scope and the check says so rather than ignoring them
quietly: no Windows job builds with a sanitizer, and their default shell is
PowerShell, so the bash wrapper has nothing to catch there. Add a sanitizer to
a Windows job and this check will demand the wrapper -- port it first.

Usage:
    python3 scripts/check_sanitizer_scan.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"
WRAPPER = "scripts/run-with-sanitizer-scan.sh"

SANITIZER_BUILD = re.compile(r"-fsanitize=")
# Commands whose output can carry a report: the suite runner, and anything
# executed straight out of the build tree.
RUNS_INSTRUMENTED_CODE = re.compile(r"(?<![\w/-])ctest\b|(?:^|\s|\./)build/\S+")
WINDOWS_RUNNER = re.compile(r"windows", re.IGNORECASE)


def _steps(job: dict) -> list[dict]:
    return [s for s in (job.get("steps") or []) if isinstance(s, dict)]


def _run_text(step: dict) -> str:
    run = step.get("run")
    return run if isinstance(run, str) else ""


def _matrix_values(job: dict) -> str:
    return yaml.safe_dump(job.get("strategy", {}))


def _is_windows(job: dict) -> bool:
    return bool(WINDOWS_RUNNER.search(str(job.get("runs-on", ""))))


def check_workflow(path: Path) -> list[str]:
    data = yaml.safe_load(path.read_text())
    if not isinstance(data, dict):
        return []
    problems: list[str] = []

    for job_name, job in (data.get("jobs") or {}).items():
        if not isinstance(job, dict):
            continue
        steps = _steps(job)
        instrumented = any(SANITIZER_BUILD.search(_run_text(s)) for s in steps)
        if not instrumented:
            continue

        if _is_windows(job):
            problems.append(
                f"{path.name}: job `{job_name}` builds with a sanitizer on a Windows "
                f"runner. {WRAPPER} is a bash script and the default shell there is "
                "PowerShell -- port the scan before enabling a sanitizer on Windows."
            )
            continue

        for step in steps:
            run = _run_text(step)
            if SANITIZER_BUILD.search(run):
                # The configure/build steps themselves produce no test output.
                continue
            for line in run.splitlines():
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                if not RUNS_INSTRUMENTED_CODE.search(stripped):
                    continue
                if stripped.startswith("cmake --build") or " -N " in f" {stripped} ":
                    # Building, or listing tests without running them.
                    continue
                if WRAPPER in stripped:
                    continue
                name = step.get("name", "<unnamed step>")
                problems.append(
                    f"{path.name}: job `{job_name}`, step `{name}` runs instrumented "
                    f"code without {WRAPPER}:\n      {stripped}"
                )
    return problems


def main() -> int:
    if not WORKFLOWS.is_dir():
        print(f"error: {WORKFLOWS} not found", file=sys.stderr)
        return 2

    wrapper = ROOT / WRAPPER
    if not wrapper.is_file():
        print(f"error: {WRAPPER} is missing", file=sys.stderr)
        return 1

    problems: list[str] = []
    checked = 0
    for path in sorted(WORKFLOWS.glob("*.yml")) + sorted(WORKFLOWS.glob("*.yaml")):
        checked += 1
        problems.extend(check_workflow(path))

    if problems:
        print(f"{len(problems)} sanitizer job(s) run without the transcript scan:\n")
        for problem in problems:
            print(f"  - {problem}")
        print(
            f"\nA sanitizer report inside a passing test is invisible to "
            f"`ctest --output-on-failure`. Run those steps through {WRAPPER}."
        )
        return 1

    print(f"every sanitizer job in {checked} workflow file(s) runs through {WRAPPER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
