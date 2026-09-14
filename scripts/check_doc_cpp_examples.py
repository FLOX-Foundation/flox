#!/usr/bin/env python3
"""scripts/check_doc_cpp_examples.py

Compile every self-contained C++ Strategy example embedded in the docs
against the real headers, using the exact same compiler flags the project
itself builds with.

Why this exists: `check_doc_symbols.py` proves a symbol a doc page names
still *exists*; it never checks whether the doc's claimed *signature* --
a field's type, a method's return type -- still matches the header.
Three reference pages survived a real signature change (two fields moving
to `std::optional`) with zero gate failures, because no gate ever fed the
doc's C++ prose to a compiler. One of the three pages showed a worked
example that summed two results directly -- valid when the return type
was a bare `double`, ill-formed once it became `std::optional<double>` --
and that example would not have compiled after the change.

Scope, stated plainly: this is NOT full signature verification across
every C++ snippet in the docs. Most `cpp` fenced blocks are fragments --
a lone field, a single method signature next to a paragraph of prose --
and compiling a fragment in isolation proves only that the *types it
names* exist, not that they match the real member's type (a fragment
redeclaring `std::optional<Price> avgEntryPrice` compiles whether or not
the real field still has that type). Closing that gap needs a real
signature diff against clang's AST for the named class, which is a
larger piece of work than this batch owns.

What this DOES close: every fenced ```cpp block that is a full,
self-contained `class ... : public Strategy { ... }` definition --
the "worked example" pattern used throughout `docs/reference/api/` and
the tutorials -- is extracted, wrapped with the real project headers,
and compiled with `-fsyntax-only`. A worked example calling a method
whose signature changed underneath it fails to compile here, same as
the real header change would break a real caller. This is real coverage
for the highest-value subset: examples readers copy-paste and expect to
compile as shown.

Compiler flags: pulled from the project's own `compile_commands.json`
(CMake's `CMAKE_EXPORT_COMPILE_COMMANDS`, already forced on at the top
of the root `CMakeLists.txt`), not hand-picked. A hand-picked flag set
drifts from the real build silently -- exactly the class of defect this
gate exists to catch, just one level up: a first version of this script
used its own `-std=gnu++2b` plus a short define list, which happened to
compile clean on one platform's toolchain and hard-errored in CI on
another (missing `-DNDEBUG` left a debug-only code path active that
isn't part of any build the project actually ships, and a warning that
is off by default in one clang was on in another). Reading the flags
from the build system makes that drift impossible by construction: if
the compiler command captured for `src/engine/symbol_registry.cpp`
lacks a flag the real build needs, the real build is missing it too.

Placeholder names in examples: a couple of worked examples call a
stand-in for the reader's own logic (`if (shouldBuy(ev)) { ... }`) that
was never part of the API. An earlier version of this script tried to
paper over that by stubbing out whatever name the compiler's error
happened to mention -- first only when Clang's exact wording matched a
regex, which meant the mechanism silently did nothing when CI's GCC
phrased the same error differently (different words, different quote
characters, no match, no stub, every placeholder example failed); then
with an explicit allowlist still triggered by parsing the compiler's
error text, which fixed the false failures but still depended on
recognizing a diagnostic's wording, compiler by compiler. Both of those
are wrong for the same reason: whether an example compiles should not
depend on guessing what a particular toolchain's error message says.
The fix that ships here removes the guessing rather than improving it --
each doc page defines its own one-line placeholder function right next
to the class that calls it (see `docs/how-to/interactive-backtest.md`
or `docs/reference/api/strategy/strategy.md` for the pattern). The
example is then a real, complete, compilable program on its own merits;
this script does not special-case any name, on any compiler, and a
reader who copies the example gets a program that builds, not a
placeholder-shaped hole. Verified against both Clang and GCC locally
before relying on CI to confirm it a second time.

Usage:
    python3 scripts/check_doc_cpp_examples.py
    python3 scripts/check_doc_cpp_examples.py --quiet
    python3 scripts/check_doc_cpp_examples.py --list   # print extracted blocks, don't compile
    FLOX_COMPILE_COMMANDS=/path/to/compile_commands.json python3 scripts/check_doc_cpp_examples.py
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

REPO_ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = REPO_ROOT / "docs"

# Headers a Strategy-example snippet plausibly needs. Broad on purpose --
# an unused include costs nothing under -fsyntax-only, and the
# alternative (guessing per-snippet) is a maintenance trap that silently
# stops checking the moment a page starts using one more class.
_COMMON_INCLUDES = (
    "flox/strategy/strategy.h",
    "flox/strategy/signal.h",
    "flox/strategy/abstract_signal_handler.h",
    "flox/backtest/backtest_runner.h",
    "flox/engine/symbol_registry.h",
    "flox/aggregator/events/bar_event.h",
    "flox/book/events/book_update_event.h",
    "flox/book/events/trade_event.h",
    "flox/execution/events/order_event.h",
    "flox/execution/order.h",
)

_FENCE_OPEN_RE = re.compile(r"^```cpp\s*$")
_FENCE_CLOSE_RE = re.compile(r"^```\s*$")

# A block only qualifies for compilation when it defines a full class
# derived from Strategy (directly or via SignalStrategy) -- the "worked
# example" shape, not a bare signature or field-list fragment.
_CLASS_DEF_RE = re.compile(
    r"\bclass\s+\w+\s*(?:final\s*)?:\s*(?:public\s+)?(?:flox::)?"
    r"(Strategy|SignalStrategy)\b"
)


@dataclass
class CppBlock:
    page: Path
    start_line: int
    code: str


def _extract_cpp_blocks(md_path: Path) -> List[CppBlock]:
    lines = md_path.read_text(errors="replace").splitlines()
    blocks: List[CppBlock] = []
    in_block = False
    start = 0
    buf: List[str] = []
    for i, line in enumerate(lines, start=1):
        if not in_block and _FENCE_OPEN_RE.match(line):
            in_block = True
            start = i
            buf = []
            continue
        if in_block and _FENCE_CLOSE_RE.match(line):
            blocks.append(CppBlock(md_path, start, "\n".join(buf)))
            in_block = False
            continue
        if in_block:
            buf.append(line)
    return blocks


def _is_compilable_example(block: CppBlock) -> bool:
    if not _CLASS_DEF_RE.search(block.code):
        return False
    # Reject obvious fragments: a full class definition balances braces
    # to zero and ends on (or near) a closing `};`.
    depth = 0
    for ch in block.code:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
    return depth == 0 and "{" in block.code


def _split_trailing_statements(code: str) -> tuple[str, str]:
    """Split off any "wiring" code a page shows after the class
    definition (construct it, attach it, run it) -- valid only inside a
    function body, not at file scope. Returns (head, trailing), where
    `head` still has the class/struct definitions and any `#include` /
    `using` lines, and `trailing` is everything after the closing `};`
    of the last top-level brace block, minus `#include`/`using` lines
    (kept in `head` instead, since those have to stay at file scope).
    """
    lines = code.splitlines()
    depth = 0
    end_of_last_top_level_block = -1
    for i, line in enumerate(lines):
        for ch in line:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
        if depth == 0 and "}" in line:
            end_of_last_top_level_block = i
    if end_of_last_top_level_block < 0:
        return code, ""
    head_lines = lines[: end_of_last_top_level_block + 1]
    rest_lines = lines[end_of_last_top_level_block + 1:]
    trailing_stmt_lines: List[str] = []
    for line in rest_lines:
        stripped = line.strip()
        if stripped.startswith("#include") or stripped.startswith("using "):
            head_lines.append(line)
        else:
            trailing_stmt_lines.append(line)
    trailing = "\n".join(trailing_stmt_lines).strip()
    return "\n".join(head_lines), trailing


def _wrap(block: CppBlock) -> str:
    includes = "\n".join(f'#include "{h}"' for h in _COMMON_INCLUDES)
    head, trailing = _split_trailing_statements(block.code)
    body = f"{head}\n"
    if trailing:
        # Free statements after the class -- the "now wire it up" half of
        # a worked example -- only parse inside a function body.
        body += f"\nstatic void flox_doc_example_entry()\n{{\n{trailing}\n}}\n"
    return (
        f"{includes}\n"
        "#include <optional>\n"
        "#include <cstdint>\n"
        "#include <iostream>\n"
        "#include <thread>\n"
        "#include <vector>\n"
        "#include <string>\n\n"
        "using namespace flox;\n\n"
        f"{body}"
    )


# ── Compiler flags: read from the real build, never hand-picked ───────

# Candidate compile_commands.json locations, checked in order. The env
# var lets a CI step that already configured a build point straight at
# it; the two build-dir names match this project's own conventions
# (`build/` from a normal local build, `build-audit/` from the audit's
# own convention) so a developer who already has one built gets instant
# reuse instead of a second throwaway configure.
_COMPILE_COMMANDS_CANDIDATES = (
    "FLOX_COMPILE_COMMANDS",  # env var name, resolved below
)
_COMPILE_COMMANDS_DIRS = ("build", "build-audit")

# A source file that is always part of the default `flox` library target
# regardless of which optional subsystems are enabled -- so a bare
# `cmake -B <dir>` configure (no extra -D flags) is guaranteed to produce
# a compile-command entry for it. Anything under `src/util/` fits: it has
# no FLOX_ENABLE_*/FLOX_BUILD_* guard anywhere in the tree.
_REPRESENTATIVE_SOURCE = "src/log/atomic_logger.cpp"


def _parse_compile_commands_entry(entries: list, repo_root: Path) -> Optional[List[str]]:
    """Return the flag list (compiler + args, minus the source file and
    -o/-c output plumbing) from the first entry whose file is under
    src/, preferring the representative source above if present."""
    candidates = [e for e in entries if "/src/" in e.get("file", "").replace("\\", "/")]
    if not candidates:
        return None
    preferred = [e for e in candidates
                 if e.get("file", "").replace("\\", "/").endswith(_REPRESENTATIVE_SOURCE)]
    entry = (preferred or candidates)[0]

    if "arguments" in entry:
        args = list(entry["arguments"])
    else:
        args = shlex.split(entry["command"])

    # Strip the flags that name this specific translation unit's input
    # and output -- everything else (defines, -I, -std, target/arch
    # flags, any -W the build sets) is exactly what we want to reuse
    # verbatim.
    flags: List[str] = [args[0]]  # the compiler itself
    i = 1
    while i < len(args):
        tok = args[i]
        if tok in ("-o", "-c"):
            i += 2 if tok == "-o" else 1
            continue
        if tok == entry.get("file"):
            i += 1
            continue
        flags.append(tok)
        i += 1
    return flags


def _load_compile_flags() -> List[str]:
    """Compiler + flags (no -fsyntax-only yet) captured from the real
    build, in this order:
      1. $FLOX_COMPILE_COMMANDS, if set, pointing at a compile_commands.json.
      2. build/compile_commands.json or build-audit/compile_commands.json,
         if either already exists (a developer's existing build tree).
      3. A throwaway `cmake -B <tmp>` configure (no build step -- CMake
         writes compile_commands.json at configure time, and the root
         CMakeLists.txt forces CMAKE_EXPORT_COMPILE_COMMANDS on already),
         torn down after reading it.
    Raises RuntimeError with a clear message if none of that produces a
    usable entry -- silently falling back to a guessed flag set is the
    exact failure mode this function exists to rule out.
    """
    env_path = os.environ.get("FLOX_COMPILE_COMMANDS")
    search_paths: List[Path] = []
    if env_path:
        search_paths.append(Path(env_path))
    for d in _COMPILE_COMMANDS_DIRS:
        search_paths.append(REPO_ROOT / d / "compile_commands.json")

    for p in search_paths:
        if p.is_file():
            try:
                entries = json.loads(p.read_text())
            except (OSError, json.JSONDecodeError):
                continue
            flags = _parse_compile_commands_entry(entries, REPO_ROOT)
            if flags:
                return flags

    # No existing build tree -- configure a throwaway one. No -D flags:
    # the whole point is the project's own defaults, the same ones any
    # plain `cmake -B build` picks up.
    cmake = os.environ.get("FLOX_CMAKE", "cmake")
    with tempfile.TemporaryDirectory() as td:
        proc = subprocess.run(
            [cmake, "-B", td, "-S", str(REPO_ROOT)],
            capture_output=True, text=True, timeout=300,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                "check_doc_cpp_examples: could not configure a build to read "
                f"compiler flags from (cmake exit {proc.returncode}):\n"
                f"{proc.stdout}\n{proc.stderr}"
            )
        cc_path = Path(td) / "compile_commands.json"
        if not cc_path.is_file():
            raise RuntimeError(
                "check_doc_cpp_examples: cmake configured but wrote no "
                f"compile_commands.json at {cc_path} -- is "
                "CMAKE_EXPORT_COMPILE_COMMANDS still forced on in the root "
                "CMakeLists.txt?"
            )
        entries = json.loads(cc_path.read_text())
        flags = _parse_compile_commands_entry(entries, REPO_ROOT)
        if not flags:
            raise RuntimeError(
                "check_doc_cpp_examples: compile_commands.json has no entry "
                f"under src/ (looked for {_REPRESENTATIVE_SOURCE}) -- the "
                "default `flox` target may have changed shape."
            )
        return flags


def _compile(base_flags: List[str], src: str, tmpdir: Path) -> tuple[bool, str]:
    src_path = tmpdir / "doc_example.cpp"
    src_path.write_text(src)
    cmd = list(base_flags) + ["-fsyntax-only", str(src_path)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    return proc.returncode == 0, proc.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--list", action="store_true",
                        help="print extracted compilable blocks, don't compile")
    args = parser.parse_args()

    doc_pages = sorted(DOCS_DIR.rglob("*.md"))
    total_cpp_blocks = 0
    all_blocks: List[CppBlock] = []
    for md_path in doc_pages:
        for block in _extract_cpp_blocks(md_path):
            total_cpp_blocks += 1
            if _is_compilable_example(block):
                all_blocks.append(block)
    fragment_count = total_cpp_blocks - len(all_blocks)

    if args.list:
        for b in all_blocks:
            print(f"{b.page.relative_to(REPO_ROOT)}:{b.start_line}")
        return 0

    # State plainly what this run does and does not cover, in numbers --
    # the same discipline check_doc_snippets.py already follows ("489
    # typed code blocks, 26 via include, 5% tested"). A gate that stays
    # quiet about what it skipped is indistinguishable from one that
    # checked everything and found nothing wrong.
    coverage_line = (
        f"check_doc_cpp_examples: scanned {len(doc_pages)} doc pages: "
        f"{total_cpp_blocks} cpp code block(s) total, {len(all_blocks)} "
        f"compiled as full self-contained Strategy examples, "
        f"{fragment_count} left unchecked as signature/field fragments "
        f"(single method signatures, struct snapshots, and similar -- "
        f"see this script's docstring for why those are out of scope)."
    )
    print(coverage_line)

    if not all_blocks:
        print("check_doc_cpp_examples: no compilable Strategy examples found "
              "-- this would be surprising; check the extraction pattern.",
              file=sys.stderr)
        return 1

    base_flags = _load_compile_flags()
    if not args.quiet:
        print(f"check_doc_cpp_examples: using compiler flags from the real "
              f"build ({base_flags[0]}, {len(base_flags) - 1} flag(s))")

    failures: List[str] = []
    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        for block in all_blocks:
            rel = block.page.relative_to(REPO_ROOT)
            ok, stderr = _compile(base_flags, _wrap(block), tmpdir)
            if ok:
                if not args.quiet:
                    print(f"OK   {rel}:{block.start_line}")
            else:
                failures.append(f"{rel}:{block.start_line}")
                print(f"FAIL {rel}:{block.start_line}", file=sys.stderr)
                print(stderr, file=sys.stderr)

    if failures:
        print(
            f"\ncheck_doc_cpp_examples: {len(failures)} example(s) do not "
            f"compile against the real headers:", file=sys.stderr,
        )
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    print(f"OK: {len(all_blocks)} of {len(all_blocks)} compilable examples "
          f"compile against the real headers "
          f"({fragment_count} fragment(s) not checked, see above).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
