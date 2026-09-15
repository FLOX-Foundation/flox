#!/usr/bin/env python3
"""Fail a test run that printed a sanitizer report, whatever its exit code said.

`ctest --output-on-failure` prints the output of the tests ctest itself
decided had failed. A sanitizer report inside a test whose assertions all
passed is therefore invisible: the summary reads `100% tests passed` and the
report scrolls by in a file nobody opens. Twice during the 2026-09 audit a
real defect surfaced exactly that way -- a data race on the logger's file
descriptor and an out-of-bounds vector read in the execution simulator --
both under fully green assertions. The command everybody runs is blind to the
one case sanitizers are run for.

Exit codes are not a reliable substitute either. Whether a report kills the
process depends on per-sanitizer options (`halt_on_error`, `abort_on_error`,
`exitcode`), on which thread the report fires on, and on whether the report
happens in a forked child whose status the parent discards. Reading the
output is the only check that does not depend on any of that.

So: scan the transcript, and go red on a report even when the run returned 0.

Usage:
    python3 scripts/scan_sanitizer_reports.py build/Testing/Temporary/LastTest.log
    python3 scripts/scan_sanitizer_reports.py --self-test

`--self-test` feeds the matcher a fabricated transcript of every report shape
it claims to catch, plus text that must not match. A detector nobody has seen
fail is not a detector.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Report shapes, keyed by the sanitizer that emits them. Anchored on the
# sanitizer's own banner rather than on loose words, so a test that merely
# prints "error" or talks about races in a log line does not match.
PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    (
        "AddressSanitizer",
        re.compile(r"(?:ERROR|SUMMARY): AddressSanitizer:"),
    ),
    (
        "LeakSanitizer",
        re.compile(r"(?:ERROR|SUMMARY): LeakSanitizer:|LeakSanitizer: detected memory leaks"),
    ),
    (
        "ThreadSanitizer",
        re.compile(r"(?:WARNING|ERROR|SUMMARY): ThreadSanitizer:"),
    ),
    (
        "MemorySanitizer",
        re.compile(r"(?:WARNING|ERROR|SUMMARY): MemorySanitizer:"),
    ),
    (
        "UndefinedBehaviorSanitizer",
        # Either the SUMMARY banner, or the per-finding line, which carries a
        # source location: `file.cpp:41:7: runtime error: ...`. Requiring the
        # location keeps an ordinary log line saying "runtime error" out.
        re.compile(r"SUMMARY: UndefinedBehaviorSanitizer:|:\d+:(?:\d+:)? runtime error: "),
    ),
]

# ctest writes one section per test into LastTest.log regardless of outcome,
# which is what makes a passing test's transcript readable at all. These two
# lines carry the name and the verdict ctest recorded for it.
TEST_HEADER = re.compile(r"^\s*\d+/\d+ Test(?:ing)?:\s*(\S+)")
TEST_VERDICT = re.compile(r"^Test (Passed|Failed|Timeout|Not Run|Skipped)")

CONTEXT_LINES = 12


class Finding:
    def __init__(self, sanitizer: str, path: Path, lineno: int, test: str | None):
        self.sanitizer = sanitizer
        self.path = path
        self.lineno = lineno
        self.test = test
        self.verdict: str | None = None
        self.context: list[str] = []


def scan_text(text: str, path: Path) -> list[Finding]:
    lines = text.splitlines()
    findings: list[Finding] = []
    current_test: str | None = None
    open_findings: list[Finding] = []

    for index, line in enumerate(lines):
        header = TEST_HEADER.match(line)
        if header:
            current_test = header.group(1)
            open_findings = []

        verdict = TEST_VERDICT.match(line)
        if verdict:
            for finding in open_findings:
                finding.verdict = verdict.group(1)
            open_findings = []

        for sanitizer, pattern in PATTERNS:
            if not pattern.search(line):
                continue
            # One report prints several matching lines (ERROR then SUMMARY).
            # Collapse them so the count reflects reports, not lines.
            if findings and findings[-1].sanitizer == sanitizer and index - findings[-1].lineno < CONTEXT_LINES * 4:
                break
            finding = Finding(sanitizer, path, index + 1, current_test)
            finding.context = lines[index : index + CONTEXT_LINES]
            findings.append(finding)
            open_findings.append(finding)
            break

    return findings


def scan_files(paths: list[Path]) -> list[Finding]:
    findings: list[Finding] = []
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"error: cannot read {path}: {exc}", file=sys.stderr)
            raise SystemExit(2)
        findings.extend(scan_text(text, path))
    return findings


def report(findings: list[Finding]) -> None:
    by_sanitizer: dict[str, int] = {}
    hidden = 0
    for finding in findings:
        by_sanitizer[finding.sanitizer] = by_sanitizer.get(finding.sanitizer, 0) + 1
        if finding.verdict == "Passed":
            hidden += 1

    print()
    print("=" * 72)
    print(f"sanitizer reports found in the test transcript: {len(findings)}")
    for sanitizer, count in sorted(by_sanitizer.items()):
        print(f"  {sanitizer}: {count}")
    if hidden:
        print(
            f"  {hidden} of them came from a test ctest recorded as Passed, so "
            "--output-on-failure would never have printed them"
        )
    print("=" * 72)

    for finding in findings:
        where = f"{finding.path}:{finding.lineno}"
        test = finding.test or "(outside any test section)"
        verdict = finding.verdict or "no verdict recorded"
        print()
        print(f"--- {finding.sanitizer} in {test} [ctest verdict: {verdict}] at {where}")
        for line in finding.context:
            print(f"    {line}")


SELF_TEST_POSITIVE = [
    ("AddressSanitizer", "==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x1"),
    ("AddressSanitizer", "SUMMARY: AddressSanitizer: heap-use-after-free pool.h:52 in flox::Pool"),
    ("LeakSanitizer", "==12345==ERROR: LeakSanitizer: detected memory leaks"),
    ("LeakSanitizer", "SUMMARY: LeakSanitizer: 64 byte(s) leaked in 1 allocation(s)."),
    ("ThreadSanitizer", "WARNING: ThreadSanitizer: data race (pid=4242)"),
    ("ThreadSanitizer", "SUMMARY: ThreadSanitizer: data race atomic_logger.cpp:72"),
    ("MemorySanitizer", "==1==WARNING: MemorySanitizer: use-of-uninitialized-value"),
    ("UndefinedBehaviorSanitizer", "src/x.cpp:41:7: runtime error: signed integer overflow"),
    ("UndefinedBehaviorSanitizer", "src/x.cpp:41: runtime error: load of misaligned address"),
    ("UndefinedBehaviorSanitizer", "SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior x.cpp:41:7"),
]

SELF_TEST_NEGATIVE = [
    "[       OK ] EventBusTest.PublishDeliversToEverySubscriber (3 ms)",
    "log: recovered from a runtime error during replay",
    "test_logger: simulating a data race to exercise the detector's own path",
    "note: ThreadSanitizer is not enabled in this configuration",
    "ERROR: connection refused by the venue gateway",
    "100% tests passed, 0 tests failed out of 248",
]


def self_test() -> int:
    failures = 0
    for expected, line in SELF_TEST_POSITIVE:
        found = scan_text(line, Path("<self-test>"))
        if len(found) != 1 or found[0].sanitizer != expected:
            got = found[0].sanitizer if found else "nothing"
            print(f"MISS  expected {expected}, matched {got}: {line}")
            failures += 1
        else:
            print(f"ok    {expected}: {line[:60]}")

    for line in SELF_TEST_NEGATIVE:
        found = scan_text(line, Path("<self-test>"))
        if found:
            print(f"FALSE POSITIVE  {found[0].sanitizer} matched: {line}")
            failures += 1
        else:
            print(f"ok    no match: {line[:60]}")

    # Attribution: a report inside a test ctest recorded as passing is the
    # whole point, so prove the verdict is carried through.
    transcript = "\n".join([
        "1/3 Testing: test_logger",
        'Command: "/build/tests/test_logger"',
        "[       OK ] LoggerTest.RotatesOnSizeLimit (1 ms)",
        "WARNING: ThreadSanitizer: data race (pid=1)",
        "[  PASSED  ] 1 test.",
        "Test Passed.",
        "2/3 Testing: test_clean",
        "Test Passed.",
    ])
    found = scan_text(transcript, Path("<self-test>"))
    if len(found) != 1 or found[0].test != "test_logger" or found[0].verdict != "Passed":
        print(f"MISS  attribution: {[(f.test, f.verdict) for f in found]}")
        failures += 1
    else:
        print("ok    attributed a report to a test ctest recorded as Passed")

    print()
    if failures:
        print(f"self-test FAILED: {failures} case(s)")
        return 1
    print("self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="*", type=Path, help="transcripts to scan")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="check the matcher against known report shapes and known non-reports",
    )
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        help="skip a transcript that does not exist instead of failing",
    )
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if not args.logs:
        parser.error("give at least one transcript, or --self-test")

    present = []
    for path in args.logs:
        if path.is_file():
            present.append(path)
        elif args.allow_missing:
            print(f"note: {path} does not exist, nothing to scan there")
        else:
            # Refusing here is deliberate. A missing transcript means the run
            # cannot be shown to be clean, and "cannot be shown" must not read
            # the same as "is clean".
            print(f"error: {path} does not exist, so this run cannot be checked", file=sys.stderr)
            return 2

    if not present:
        print("error: no transcript to scan", file=sys.stderr)
        return 2

    findings = scan_files(present)
    if not findings:
        scanned = ", ".join(str(p) for p in present)
        print(f"no sanitizer reports in {scanned}")
        return 0

    report(findings)
    return 1


if __name__ == "__main__":
    sys.exit(main())
