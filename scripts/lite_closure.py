#!/usr/bin/env python3
"""The direction gate for the venue-lite build profile.

`venue/lite_surface.txt` is the list of headers a downstream consumer that
runs the execution venue compiles against. What that list is *allowed to
reach* is the interesting part: a header can be on the list, look small, and
drag the backtest module, the aggregator, or an exchange connector in behind
it. The profile that claims to build without those is only as honest as its
transitive closure.

So this walks the closure of the list's `#include "flox/..."` and
`#include "flox-venue/..."` edges -- `.inl` bodies included, since the
headers include them -- and fails when the closure reaches somewhere the
profile forbids:

  * flox/backtest/*      -- the profile builds with FLOX_ENABLE_BACKTEST=OFF
  * flox/aggregator/*    -- the venue has no bars
  * flox/connector/*     -- except flox/connector/fix/*, the initiator
  * flox-connectors/*    -- the exchange connector module
  * anything outside the allowed directories below

Exit 0 prints the closure and its size. Exit 1 names each offending header
and the include chain that reached it, because "flox/backtest/account.h is in
the closure" without the path to it is not actionable.

`--paths` prints the same closure as repository-relative file paths and
nothing else, and runs the direction gate but not the list/TU cross-check. The lite profile's install rules read it at configure time, so
what that profile installs IS the closure rather than a second list somebody
has to keep in step with the first. The gate still runs: a closure that
fails it fails the configure.
"""
import os
import re
import sys
from collections import deque

ROOTS = ["include", "venue/include"]
SURFACE = os.path.join("venue", "lite_surface.txt")
SURFACE_TU = os.path.join("venue", "tests", "support", "lite_surface.cpp")

INCLUDE = re.compile(r'^\s*#\s*include\s+[<"]((?:flox|flox-venue)/[^">]+)[>"]')

# Where the closure may go. A directory that is not here is a new dependency
# direction, and the profile wants that decided in a review rather than
# discovered by a consumer.
ALLOWED = [
    "flox-venue/",
    "flox/book/",
    "flox/clearing/",
    "flox/common.h",
    "flox/connector/fix/",
    "flox/engine/",
    "flox/execution/",
    "flox/log/",
    "flox/net/",
    "flox/util/",
]

# Checked before ALLOWED so the message says which rule was broken rather
# than the generic one.
FORBIDDEN = [
    ("flox/backtest/", "the lite profile builds with FLOX_ENABLE_BACKTEST=OFF"),
    ("flox/aggregator/", "venue subscribers do not carry bars"),
    ("flox-connectors/", "exchange connectors are not in the lite profile"),
]


def resolve(path):
    for root in ROOTS:
        candidate = os.path.join(root, path)
        if os.path.isfile(candidate):
            return candidate
    return None


def read_tu_includes(path):
    """The flox / flox-venue includes of the surface translation unit.

    engine_surface.h is included by relative path and is not part of the
    consumer's list, so it does not match and is not counted.
    """
    found = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            match = INCLUDE.match(line)
            if match:
                found.append(match.group(1))
    return found


def read_surface(path):
    entries = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if line:
                entries.append(line)
    return entries


def verdict(header):
    for prefix, why in FORBIDDEN:
        if header.startswith(prefix):
            return why
    if header.startswith("flox/connector/") and not header.startswith(
        "flox/connector/fix/"
    ):
        return "only the FIX initiator is in the lite profile"
    for prefix in ALLOWED:
        if header == prefix or header.startswith(prefix):
            return None
    return "outside the directories the lite profile allows"


def closure_of(seeds):
    """Breadth-first walk of the #include graph starting at `seeds`.

    Shared by the consumer-surface closure below and by
    scripts/mirror_paths.py's wider closure over the whole venue module --
    one walk, one direction gate (`verdict`), so the two closures cannot
    silently disagree about what "reaches a forbidden directory" means.

    Returns (closure, missing, violations): `closure` is the sorted set of
    every header reached (including the seeds), `missing` is
    (header, includer) pairs for headers that do not resolve under
    include/ or venue/include/, and `violations` is (header, reason) pairs
    for headers `verdict` rejects.
    """
    seen = {}
    queue = deque()
    for header in seeds:
        if header not in seen:
            seen[header] = None
            queue.append(header)

    missing = []
    while queue:
        header = queue.popleft()
        resolved = resolve(header)
        if resolved is None:
            missing.append((header, seen[header]))
            continue
        with open(resolved, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = INCLUDE.match(line)
                if not match:
                    continue
                target = match.group(1)
                if target not in seen:
                    seen[target] = header
                    queue.append(target)

    closure = sorted(seen)
    violations = [(h, verdict(h)) for h in closure if verdict(h) is not None]
    return closure, missing, violations


def main():
    paths_only = "--paths" in sys.argv[1:]

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(repo)

    surface = read_surface(SURFACE)

    # The list has one home. The translation unit that proves it compiles
    # must include exactly it -- otherwise the contract is two lists, and
    # the one nobody edits is the one that goes stale.
    #
    # Not under --paths: that call answers "what does the lite profile
    # install", and it runs at configure time. Failing it there would mean a
    # broken contract stops the configure, and the surface target nobody can
    # build is a worse report than the surface target that goes red.
    # Compared as sets: clang-format sorts includes inside a block, so the
    # TU's order is the formatter's and not the list's. What has to match is
    # which headers, not in which order.
    tu = read_tu_includes(SURFACE_TU)
    if not paths_only and sorted(tu) != sorted(surface):
        missing_in_tu = [h for h in surface if h not in tu]
        extra_in_tu = [h for h in tu if h not in surface]
        print("%s and %s disagree" % (SURFACE, SURFACE_TU))
        for header in missing_in_tu:
            print("  on the list, not included by the TU: %s" % header)
        for header in extra_in_tu:
            print("  included by the TU, not on the list: %s" % header)
        return 1

    seen = {}
    queue = deque()
    missing = []

    for header in surface:
        if header not in seen:
            seen[header] = None
            queue.append(header)

    while queue:
        header = queue.popleft()
        resolved = resolve(header)
        if resolved is None:
            missing.append((header, seen[header]))
            continue
        with open(resolved, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = INCLUDE.match(line)
                if not match:
                    continue
                target = match.group(1)
                if target not in seen:
                    seen[target] = header
                    queue.append(target)

    closure = sorted(seen)

    def chain(header):
        steps = [header]
        parent = seen.get(header)
        while parent is not None:
            steps.append(parent)
            parent = seen.get(parent)
        return " <- ".join(steps)

    violations = []
    for header in closure:
        why = verdict(header)
        if why is not None:
            violations.append((header, why))

    if not violations and not missing and paths_only:
        for header in closure:
            print(resolve(header))
        return 0

    for header in closure:
        print(header)

    venue_side = sum(1 for h in closure if h.startswith("flox-venue/"))
    print()
    print(
        "closure: %d headers from %d direct includes "
        "(%d flox/, %d flox-venue/)"
        % (len(closure), len(surface), len(closure) - venue_side, venue_side)
    )

    if missing:
        print()
        for header, parent in missing:
            where = "venue/lite_surface.txt" if parent is None else parent
            print("MISSING: %s, included by %s" % (header, where))

    if violations:
        print()
        for header, why in violations:
            print("FORBIDDEN: %s -- %s" % (header, why))
            print("           %s" % chain(header))
        print()
        print("%d header(s) the lite profile does not allow" % len(violations))
        return 1

    if missing:
        print()
        print("%d header(s) on the surface do not exist" % len(missing))
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
