#!/usr/bin/env python3
"""An exception leaving a thread body is std::terminate for the whole process.

The rule is that every thread in the framework is built through
flox::makeThread, which contains the body and reports the death. The rule was
written out three separate times in three separate files before it was applied
everywhere, so this check exists to keep it from drifting apart again.

Tests are exempt: they create threads to exercise the containment itself, and
a test that dies loudly is a test doing its job.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROOTS = ["include", "src", "venue", "connectors", "tools", "demo", "benchmarks"]
SKIP_DIRS = {"external", "_deps"}

# The one place allowed to say std::thread: the helper that wraps it.
ALLOWED = {Path("include/flox/util/concurrency/thread_body.h")}

# Construction, in any of its spellings: std::thread(...), std::thread t(...),
# std::thread t{...}. A bare declaration (std::thread t;), a container type
# (vector<std::thread>) and a parameter (void f(std::thread t)) are none of
# those and must not be flagged, or the check becomes noise and gets disabled.
THREAD = re.compile(r"\bstd::(?:thread|jthread)\s*\w*\s*[({]")
# A thread can also be created without ever naming the type: emplace_back on a
# container of them constructs one in place. push_back(makeThread(...)) is the
# sanctioned form, so only emplace_back is flagged.
CONTAINER = re.compile(r"(?:vector|deque)\s*<\s*std::j?thread\s*>\s*(\w+)")


def sources():
    for root in ROOTS:
        base = ROOT / root
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in (".h", ".hpp", ".cpp", ".cc", ".ipp"):
                continue
            parts = set(path.parts)
            if parts & SKIP_DIRS or any(p.startswith("build") for p in path.parts):
                continue
            if "tests" in path.parts or "test" in path.parts:
                continue
            yield path


def main() -> int:
    bad = []
    for path in sources():
        rel = path.relative_to(ROOT)
        if rel in ALLOWED:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in THREAD.finditer(text):
            # A declaration (a member, a container of them) is not a thread
            # body; only construction is.
            line = text[text.rfind("\n", 0, m.start()) + 1 : text.find("\n", m.start())]
            if re.search(r"(?:vector|deque|array|optional)\s*<\s*std::j?thread", line):
                continue
            if re.match(r"\s*(?:std::)?j?thread\s+\w+\s*;", line.strip()):
                continue
            bad.append((rel, text[: m.start()].count("\n") + 1, line.strip()))

        for name in set(CONTAINER.findall(text)):
            for m in re.finditer(rf"\b{re.escape(name)}\.emplace_back\s*\(", text):
                line = text[text.rfind("\n", 0, m.start()) + 1 : text.find("\n", m.start())]
                bad.append((rel, text[: m.start()].count("\n") + 1, line.strip()))

    if bad:
        print("std::thread constructed directly; use flox::makeThread so an")
        print("exception leaving the body cannot terminate the process:")
        for rel, line, src in bad:
            print(f"  {rel}:{line}: {src}")
        return 1
    print(f"thread bodies contained: no direct std::thread construction outside "
          f"{', '.join(str(p) for p in sorted(ALLOWED))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
