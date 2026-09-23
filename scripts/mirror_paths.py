#!/usr/bin/env python3
"""The path list of the self-contained venue-core tree.

T013's step 3 turns the venue module into a derived, read-only mirror: CI
cuts the paths this script prints out of the monorepo (a `git subtree
split`-shaped operation) and publishes them, with history, to a separate
repository. Before that repository exists, the question the rehearsal in the
task asks is simpler and answerable today: does a tree made of *only* these
paths configure, build and pass the venue-lite profile's tests on its own,
with nothing reached by relative `../` or by a stray absolute path back into
the rest of this repo? `scripts/lite_standalone_check.sh` copies exactly this
list into a scratch directory and runs the profile there -- so this script is
the one place that says what "the mirror" means, and the checklist in
.notes/tracks/W9-build-and-repo-hygiene/T013-venue-lite-profile.md points
here rather than keeping its own copy.

Two closures are involved, not one:

* `scripts/lite_closure.py --paths` is the *consumer* closure -- the 37
  headers in venue/lite_surface.txt (38 with the one entry marked "required
  by lite library") and everything they transitively include. That is
  deliberately narrow: it is what `cmake --install` ships, and it is what
  decides which core `src/**/*.cpp` the lite profile compiles.
* The venue module's *own* source -- its library, its tests, its benchmarks
  -- reaches further than that. A venue test that includes
  `flox-venue/control_server.h` (perimeter admin surface) needs headers no
  downstream consumer's 38-entry list ever mentions, because the consumer
  never uses that class. A first rehearsal of this cut (git filter-repo into
  a local copy, three runs) found this out by building: with only the
  consumer closure's headers present, `venue/tests/test_venue_admin_surface.cpp`
  failed to compile on a missing header, not on a missing symbol. The mirror
  is the venue library plus its test suite, not the consumer's 38-header
  contract -- so `mirror_header_closure()` below walks the union of the
  consumer closure and everything venue/src, venue/tests (minus the three
  files the lite profile itself never builds -- see `dropped_in_lite`) and
  venue/benchmarks actually `#include`. Both walks share one BFS and one
  direction gate (`lite_closure.resolve` / `lite_closure.verdict`), imported
  from scripts/lite_closure.py rather than re-implemented here, so the two
  closures cannot silently disagree about what "reaches flox/backtest/" means.

`venue/include/` itself is copied whole, closure or not. Two headers there
(`tape_recorder.h`, `liquidation_monitor.h`) reach `flox/replay/` and
`flox/backtest/` respectively and are never included by anything the lite
profile compiles -- their only includers are the two venue tests the profile
already excludes (`test_venue_tape`, `test_venue_derivatives`, both dropped
by the same rule venue/CMakeLists.txt uses). Leaving those two headers out of
the mirror would make it a different, smaller module than the one flox ships;
leaving their forbidden includes out of the *closure* is what keeps the
direction gate meaningful. A venue test that reached a forbidden directory
through some *other*, not-already-excluded path would be a real finding here
-- see `mirror_header_closure`'s violations output -- and the fix is to
exclude that test with a documented reason next to the three that are
already excluded, not to widen the mirror's allowed directories.

Usage:
    python3 scripts/mirror_paths.py             # the path list, one per line
    python3 scripts/mirror_paths.py --summary    # counts and the direction
                                                  # gate's verdict, on stderr,
                                                  # then the path list

Exit 0 and the path list on stdout, or exit 1 and a diagnosis (same shape as
scripts/lite_closure.py's) if the wider closure reaches a directory the lite
profile forbids or a header this repository does not have.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lite_closure as lc  # noqa: E402  (path insert must run first)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The same three rules venue/CMakeLists.txt applies to FLOX_VENUE_TESTS in
# the lite profile (FLOX_FIX_TLS off by default, FLOX_ENABLE_BACKTEST off,
# FLOX_VENUE_LITE builds no replay writers) -- restated here because a test
# file dropped from the *build* must also be dropped from what *seeds* the
# mirror's header closure, or the closure would demand headers the lite
# profile never actually needs to compile.
DROP_PATTERNS = [
    (re.compile(r"fix_tls_channel\.h"), "FLOX_FIX_TLS is off by default"),
    (re.compile(r"flox/backtest/|liquidation_monitor\.h"), "FLOX_ENABLE_BACKTEST=OFF in the lite profile"),
    (re.compile(r"flox/replay/|tape_recorder\.h"), "FLOX_VENUE_LITE builds no replay writers"),
]

# Top-level files copied whole. Directories copied whole are listed
# separately, in collect_paths, next to the reason each one is in scope.
WHOLE_FILES = [
    "venue/CMakeLists.txt",
    "venue/lite_surface.txt",
    "venue/README.md",
    "scripts/lite_closure.py",
    "scripts/mirror_paths.py",
    "scripts/lite_standalone_check.sh",
    "CMakeLists.txt",
    "CMakePresets.json",
    "LICENSE",
    ".clang-format",
    # The lite jobs live in ci.yml alongside ~40 others that reference
    # directories this tree does not have (python/, connectors/, node/...).
    # Slicing the file down to just those jobs is publish-time work for
    # whoever cuts the real mirror; this rehearsal only has to prove the
    # *source* tree builds, and a workflow file that a cmake configure never
    # reads cannot break that -- so it is carried whole, not trimmed.
    ".github/workflows/ci.yml",
]

# Directories copied whole (every file under them, not filtered by any
# closure): the venue module is its source tree, not just what one consumer
# includes.
WHOLE_DIRS = [
    "venue/include",
    "venue/src",
    "venue/tests",
    "venue/benchmarks",
    "cmake",
    "docs/venue",
]


def dropped_in_lite(path):
    """Why the lite profile drops this venue/tests/*.cpp file, or None."""
    with open(path, encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    for pattern, why in DROP_PATTERNS:
        if pattern.search(text):
            return why
    return None


def _files_under(rel_dir, suffixes):
    out = []
    base = os.path.join(REPO, rel_dir)
    for dirpath, _dirnames, filenames in os.walk(base):
        for name in filenames:
            if name.endswith(suffixes):
                out.append(os.path.relpath(os.path.join(dirpath, name), REPO))
    return sorted(out)


def _all_files_under(rel_dir):
    """Every file under rel_dir, no suffix filter -- for WHOLE_DIRS, where
    "whole" means whole: CMakeLists.txt, .cmake.in templates, golden .txt
    tables, everything.
    """
    out = []
    base = os.path.join(REPO, rel_dir)
    for dirpath, _dirnames, filenames in os.walk(base):
        for name in filenames:
            out.append(os.path.relpath(os.path.join(dirpath, name), REPO))
    return sorted(out)


def mirror_header_closure():
    """(closure, missing, violations, dropped) for the wider, venue-module
    closure described in the module docstring above. `closure` is include/
    and venue/include/-relative header paths (like lite_closure.resolve
    expects), sorted; `dropped` maps a venue/tests/*.cpp path to the reason
    it did not seed the walk.
    """
    seeds = set(lc.read_surface(lc.SURFACE))

    tu_files = (
        _files_under("venue/src", (".cpp",))
        + [p for p in _files_under("venue/tests", (".cpp",)) if os.path.dirname(p) == "venue/tests"]
        + _files_under("venue/benchmarks", (".cpp",))
    )

    dropped = {}
    for rel in tu_files:
        path = os.path.join(REPO, rel)
        if os.path.dirname(rel) == "venue/tests":
            why = dropped_in_lite(path)
            if why:
                dropped[rel] = why
                continue
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = lc.INCLUDE.match(line)
                if match:
                    seeds.add(match.group(1))

    closure, missing, violations = lc.closure_of(seeds)
    return closure, missing, violations, dropped


def lite_cpp_sources(narrow_closure):
    """Core src/{clearing,engine,execution,log}/*.cpp the lite profile
    compiles -- the same filter the root CMakeLists.txt applies under
    FLOX_VENUE_LITE: a .cpp is kept iff its own header (src/x/y.cpp ->
    include/flox/x/y.h) is in the narrow, consumer-plus-link-requirements
    closure. `narrow_closure` is scripts/lite_closure.py's closure (header
    keys, e.g. "flox/clearing/account.h" -- not resolved paths).
    """
    kept = []
    for rel_dir in ("src/clearing", "src/engine", "src/execution", "src/log"):
        for cpp in _files_under(rel_dir, (".cpp",)):
            header_key = "flox/" + cpp[len("src/") :][: -len(".cpp")] + ".h"
            if header_key in narrow_closure:
                kept.append(cpp)
    return sorted(kept)


def collect_paths():
    """(paths, diagnosis). `paths` is the sorted, de-duplicated mirror file
    list on success; on failure it is None and `diagnosis` is the lines to
    print instead (same shape as scripts/lite_closure.py's failure output).
    """
    wide_closure, missing, violations, dropped = mirror_header_closure()

    if missing or violations:
        lines = []
        if missing:
            for header, parent in missing:
                where = "venue/lite_surface.txt" if parent is None else parent
                lines.append("MISSING: %s, included by %s" % (header, where))
        if violations:
            lines.append("")
            for header, why in violations:
                lines.append("FORBIDDEN: %s -- %s" % (header, why))
            lines.append("")
            lines.append(
                "%d header(s) the mirror's wider closure does not allow "
                "-- exclude the venue test that reaches it (see "
                "dropped_in_lite / DROP_PATTERNS), do not widen ALLOWED"
                % len(violations)
            )
        return None, lines

    narrow_closure = set(lc.read_surface(lc.SURFACE))
    narrow_closure, narrow_missing, narrow_violations = lc.closure_of(narrow_closure)
    if narrow_missing or narrow_violations:
        # scripts/lite_closure.py itself would already have failed the
        # configure; report it here too rather than produce a silently
        # incomplete .cpp list.
        return None, ["the consumer closure itself is broken; run "
                       "scripts/lite_closure.py for the diagnosis"]

    paths = set()

    for rel in WHOLE_FILES:
        if os.path.isfile(os.path.join(REPO, rel)):
            paths.add(rel)

    for rel_dir in WHOLE_DIRS:
        paths.update(_all_files_under(rel_dir))

    for header in wide_closure:
        resolved = lc.resolve(header)
        if resolved:
            paths.add(os.path.relpath(resolved, REPO))

    paths.update(lite_cpp_sources(narrow_closure))

    return sorted(paths), {
        "wide_closure": len(wide_closure),
        "narrow_closure": len(narrow_closure),
        "dropped": dropped,
    }


def main():
    summary = "--summary" in sys.argv[1:]

    os.chdir(REPO)
    paths, extra = collect_paths()

    if paths is None:
        for line in extra:
            print(line, file=sys.stderr)
        return 1

    if summary:
        print("mirror paths: %d files" % len(paths), file=sys.stderr)
        print(
            "header closures: %d (consumer/link) / %d (venue module, wider)"
            % (extra["narrow_closure"], extra["wide_closure"]),
            file=sys.stderr,
        )
        for rel, why in sorted(extra["dropped"].items()):
            print("not seeding the closure: %s -- %s" % (rel, why), file=sys.stderr)

    for path in paths:
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
