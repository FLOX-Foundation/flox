#!/usr/bin/env python3
"""Every traversal of an unordered container in venue/include must say why.

A std::unordered_map enumerates in hash-bucket order. Bucket order is not a
property of the data -- it is a property of the library implementation, the
insertion history and the rehash points, so two venues built from the same
source against libc++ and libstdc++ walk the same state in different orders.

That is harmless right up to the point where the order reaches something
observable: an event published to a sink, the bytes of a snapshot or a
sidecar, a determinism hash, or the choice of which candidate gets liquidated,
deleveraged, expired or pulled first. Then the venue's OUTPUT depends on which
standard library it was compiled with, while its STATE stays identical -- a
divergence every state-comparison test in the suite is blind to, and one the
golden replay (venue/tests/test_venue_golden_replay.cpp) only caught because it
runs on both libraries against a written-down table. It found exactly one:
StopBook::ids() publishing an emergency cancel in bucket order (2c5224d0).

So the rule here is not "sort everything" -- most traversals fold
commutatively, count, or answer a yes/no, and sorting those would cost time
and say nothing. The rule is that each traversal is ANNOTATED with the verdict
someone reached about it, in a `// order: ...` comment on the traversal or
within the two lines above it:

    // order: not observable -- integer sum, addition is associative
    for (const auto& [acct, p] : positions_)

    // order: sorted below, before the vector is handed out
    for (const auto& [id, _] : instruments_)

A new traversal with no verdict fails the build. That is the point: the next
one of these is written by someone who did not know the rule existed, and the
five seconds it takes to write the comment is when they find out whether their
order is observable.

Scope is venue/include, plus include/flox/position and include/flox/backtest:
a backtest is expected to reproduce bit-for-bit on another machine, and
W32-T012 found a float fold over unordered_* there that libc++ and libstdc++
walk in different orders (include/flox/position/portfolio_greeks.h) and
audited the rest of both directories for the same shape of bug. Each root is
scanned as headers and the *.inl fragments they include alike -- a fragment
declares no members of its own, so it inherits the declarations of the header
that includes it: otherwise moving a traversal out of a class body into an
.inl would quietly drop its verdict duty. Tests are exempt: a test asserting a
property of bucket order is the thing doing the checking.

The check is a drift guard, not a prover. It recognises the traversal spellings
the tree actually uses -- range-for over a named unordered container, begin()
on one, erase_if over one, range-for over the inner container of a nested
unordered map, and range-for over an accessor that returns an unordered
container by reference. A traversal reached some other way still needs the same
verdict; nothing here can find it for you.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCAN_ROOTS = [
    ROOT / "venue" / "include",
    ROOT / "include" / "flox" / "position",
    ROOT / "include" / "flox" / "backtest",
]

UNORDERED = r"std::unordered_(?:map|set|multimap|multiset)"

# A member, local or parameter whose type is an unordered container:
#   std::unordered_map<K, V> name_;   const std::unordered_set<T>& name)
DECL = re.compile(rf"{UNORDERED}\s*<.*?>\s*&?\s*([A-Za-z_]\w*)\s*(?:;|=|\{{|\)|,)")
# A map whose MAPPED type is itself unordered: the inner container is reached
# as `it->second` / `a->second`, never by name.
NESTED = re.compile(rf"{UNORDERED}\s*<[^,]+,\s*{UNORDERED}\s*<")
# An accessor handing an unordered container out by reference.
ACCESSOR = re.compile(rf"(?:const\s+)?{UNORDERED}\s*<.*?>\s*&\s*([A-Za-z_]\w*)\s*\(")

ORDER_NOTE = re.compile(r"//.*\border:\s*\S")

# How far above the traversal the note may sit. Two lines is enough for a
# comment that wrapped; more and the note stops being next to the loop.
LOOKBACK = 2


# An .inl is a fragment of the header that includes it, not a file of its own.
INCLUDE = re.compile(r'#\s*include\s+"([^"]+)"')


def headers():
    seen = set()
    for scan in SCAN_ROOTS:
        found = list(scan.rglob("*.h")) + list(scan.rglob("*.inl"))
        for path in sorted(found):
            if "tests" in path.parts:
                continue
            if path in seen:
                continue
            seen.add(path)
            yield path


def including(texts):
    """Map each .inl to the headers that include it."""
    owners: dict[Path, set[Path]] = {}
    for path, text in texts.items():
        if path.suffix != ".inl":
            continue
        posix = path.as_posix()
        for other, otext in texts.items():
            if other is path:
                continue
            if any(posix.endswith(inc) for inc in INCLUDE.findall(otext)):
                owners.setdefault(path, set()).add(other)
    return owners


def traversals(lines: list[str], names: set[str], foreign: set[str], nested: bool,
               accessors: set[str]):
    """Yield (line_index, spelling) for each traversal that needs a verdict.

    `names` are declared in this header and match bare; `foreign` are declared
    in another one and match only through a qualifier (`sample.byMaker`), or a
    local named `out` would inherit the verdict duty of an unrelated
    `unordered_map& out` parameter three headers away.
    """
    for i, line in enumerate(lines):
        code = line.split("//", 1)[0]
        if not code.strip():
            continue
        for name, qualified in [(n, False) for n in names] + [(n, True) for n in foreign]:
            n = re.escape(name)
            q = r"[\w\]\)]\s*(?:\.|->)" if qualified else r"(?:[\w.\-> ]*\.)?"
            if re.search(rf"for\s*\(.*:\s*{q}{n}\s*\)", code):
                yield i, f"range-for over {name}"
                break
            if re.search(rf"{q}{n}\.c?begin\s*\(\)" if qualified
                         else rf"\b{n}\.c?begin\s*\(\)", code):
                yield i, f"{name}.begin()"
                break
            if re.search(rf"erase_if\s*\(\s*{q}{n}\b", code):
                yield i, f"erase_if over {name}"
                break
        else:
            if nested and re.search(r"[\w]+\s*(?:->|\.)second\s*\.c?begin\s*\(\)", code):
                yield i, "begin() on the inner unordered container"
                continue
            if nested and re.search(r"for\s*\(.*:\s*[\w]+\s*(?:->|\.)second\s*\)", code):
                yield i, "range-for over the inner unordered container"
                continue
            for acc in accessors:
                if re.search(rf"for\s*\(.*:\s*[\w.\->]*\b{re.escape(acc)}\s*\(\s*\)\s*\)", code):
                    yield i, f"range-for over {acc}()"
                    break


def main() -> int:
    missing = [scan for scan in SCAN_ROOTS if not scan.is_dir()]
    if missing:
        for scan in missing:
            print(f"check_iteration_order: {scan} not found")
        return 1

    # Accessors are collected across the whole tree: a header traverses the one
    # another header hands out.
    accessors: set[str] = set()
    texts: dict[Path, str] = {}
    declared: set[str] = set()
    for path in headers():
        text = path.read_text(encoding="utf-8", errors="replace")
        texts[path] = text
        accessors.update(ACCESSOR.findall(text))
        declared.update(DECL.findall(text))

    owners = including(texts)

    bad = []
    seen = 0
    for path, text in texts.items():
        # Names declared here match bare. Every other unordered member name in
        # the tree matches only through a qualifier: one header traverses a
        # struct another header declares (LastLookSample::byMaker, walked by
        # the Prometheus exposition).
        names = set(DECL.findall(text))
        family = [texts[o] for o in owners.get(path, ())]
        for otext in family:
            names |= set(DECL.findall(otext))
        foreign = declared - names
        lines = text.split("\n")
        nested = any(NESTED.search(t) is not None for t in [text] + family)
        for i, what in traversals(lines, names, foreign, nested, accessors):
            seen += 1
            window = lines[max(0, i - LOOKBACK): i + 1]
            if any(ORDER_NOTE.search(w) for w in window):
                continue
            bad.append((path.relative_to(ROOT), i + 1, what, lines[i].strip()))

    if bad:
        print("unordered traversal with no `// order: ...` verdict above it.")
        print("Bucket order is not a property of the data. Say whether it")
        print("reaches a publication, a snapshot, a hash or a choice of")
        print("candidate -- and if it does, sort at the accessor:")
        for rel, line, what, src in bad:
            print(f"  {rel}:{line}: {what}")
            print(f"      {src}")
        return 1
    scanned = ", ".join(str(scan.relative_to(ROOT)) for scan in SCAN_ROOTS)
    print(f"iteration order: {seen} unordered traversals in "
          f"{scanned}, every one with a verdict")
    return 0


if __name__ == "__main__":
    sys.exit(main())
