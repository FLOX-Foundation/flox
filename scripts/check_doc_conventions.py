#!/usr/bin/env python3
"""scripts/check_doc_conventions.py

Enforce the naming / packaging conventions that hand-written docs keep
getting wrong. Every rule below has burned a reader: the snippet looks
plausible, copy-pastes cleanly, and fails on line one.

Rules:

* `PY_IMPORT`   — `import flox` as a bare Python import. The distribution
                  installs the package as `flox_py`; docs alias it with
                  `import flox_py as flox`. `import flox` is an ImportError.
* `NODE_PKG`    — `require('flox')`, `require('flox-node')`,
                  `from "flox"`. The npm package is
                  `@flox-foundation/flox`; the others do not resolve.
* `QUICKJS_REQ` — `require(` inside a `docs/reference/quickjs/**` page. The
                  embedded runtime has no module loader; classes arrive as
                  globals. Lines that explicitly state the *absence* of
                  `require()` are allowed.
* `CMAKE_FLAG`  — the deprecated `FLOX_ENABLE_{TESTS,BENCHMARKS,DEMO,TOOLS,
                  PYTHON,NODE,CAPI,CODON,QUICKJS}` options inside a runnable
                  shell / cmake block. Prose that documents the rename (the
                  alias table in `docs/build/feature-flags.md`) is fine.
* `EMOJI`       — any emoji. The project forbids them. `✓` (U+2713) and
                  `✗` (U+2717) are legitimate table markers and are not
                  emoji for this gate's purposes.

Exemptions live in `_EXEMPTIONS` below, keyed by (page, rule). Each carries
a reason; anything that is a real defect owned by another file carries a
`TODO:` so it is greppable.

Usage:
    python3 scripts/check_doc_conventions.py            # report + exit 1
    python3 scripts/check_doc_conventions.py --quiet     # only failures
    python3 scripts/check_doc_conventions.py --rules EMOJI,PY_IMPORT
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = REPO_ROOT / "docs"

# Generated aggregates of the whole doc tree: fixing a violation there means
# fixing its source page, which the other rules already cover.
EXCLUDED_NAMES = {"llms.txt", "llms-full.txt"}

# Fence languages that hold commands a reader will paste into a shell.
_SHELL_LANGS = {"bash", "sh", "shell", "shell-session", "console", "cmake",
                "text", ""}

_FENCE_RE = re.compile(r"^\s*(?P<mark>`{3,}|~{3,})(?P<lang>[A-Za-z0-9_+-]*)")

_PY_IMPORT_RE = re.compile(r"^\s*import\s+flox\s*(?:as\s+\w+)?\s*(?:#.*)?$")
_NODE_PKG_RE = re.compile(
    r"""require\(\s*['"]flox(?:-node)?['"]\s*\)"""
    r"""|from\s+['"]flox(?:-node)?['"]"""
)
_REQUIRE_RE = re.compile(r"\brequire\s*\(")
_NO_REQUIRE_RE = re.compile(r"(?i)\b(?:no|not|without)\b[^.]{0,60}require\s*\(")
_CMAKE_FLAG_RE = re.compile(
    r"FLOX_ENABLE_(?:TESTS|BENCHMARKS|DEMO|TOOLS|PYTHON|NODE|CAPI|CODON"
    r"|QUICKJS)\b"
)

# Emoji, deliberately narrow: pictographs, dingbats, misc symbols, regional
# indicators, variation selector 16. U+2713 / U+2717 are excluded because
# docs use them as table markers, and the geometric shapes / box drawing /
# arrow blocks are excluded because ASCII-art diagrams use them.
_EMOJI_RANGES = (
    (0x1F000, 0x1FAFF),
    (0x2600, 0x26FF),
    (0x2700, 0x27BF),
    (0xFE0F, 0xFE0F),
    (0x1F1E6, 0x1F1FF),
)
_EMOJI_SINGLES = {0x2B05, 0x2B06, 0x2B07, 0x2B1B, 0x2B1C, 0x2B50, 0x2B55,
                  0x203C, 0x2049, 0x2139}
_EMOJI_EXEMPT = {0x2713, 0x2717, 0x2714, 0x2718}

RULES = ("PY_IMPORT", "NODE_PKG", "QUICKJS_REQ", "CMAKE_FLAG", "EMOJI")

# (page relative to repo root, rule) -> reason. Keep narrow: one page, one
# rule. A `TODO:` marks a real defect this gate cannot fix from here.
_EXEMPTIONS: Dict[Tuple[str, str], str] = {}

_FIX_HINT = {
    "PY_IMPORT": "use `import flox_py as flox` (the installed package is "
                 "flox_py)",
    "NODE_PKG": "use `require('@flox-foundation/flox')` / "
                "`from \"@flox-foundation/flox\"`",
    "QUICKJS_REQ": "drop the require: the embedded runtime injects classes "
                   "as globals",
    "CMAKE_FLAG": "use the FLOX_BUILD_* name (FLOX_ENABLE_* is a deprecated "
                  "alias that warns and will be removed)",
    "EMOJI": "delete it: the project forbids emoji in docs and code",
}


def _is_emoji(ch: str) -> bool:
    cp = ord(ch)
    if cp in _EMOJI_EXEMPT:
        return False
    if cp in _EMOJI_SINGLES:
        return True
    return any(lo <= cp <= hi for lo, hi in _EMOJI_RANGES)


def _scan(path: Path, rel: str, enabled: Set[str]) -> List[Tuple[str, int, str]]:
    """Return [(rule, line_no, detail)] for one page."""
    out: List[Tuple[str, int, str]] = []
    in_quickjs = rel.startswith("docs/reference/quickjs/")
    fence_lang: str | None = None
    fence_mark = ""

    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        fence = _FENCE_RE.match(line)
        if fence:
            if fence_lang is None:
                fence_lang = fence.group("lang").lower()
                fence_mark = fence.group("mark")
            elif (not fence.group("lang")
                    and fence.group("mark")[0] == fence_mark[0]
                    and len(fence.group("mark")) >= len(fence_mark)):
                fence_lang = None
            continue

        in_fence = fence_lang is not None

        if "EMOJI" in enabled:
            for ch in line:
                if _is_emoji(ch):
                    out.append(("EMOJI", lineno,
                                f"U+{ord(ch):04X} in {line.strip()[:60]!r}"))
                    break

        if "PY_IMPORT" in enabled and _PY_IMPORT_RE.match(line):
            out.append(("PY_IMPORT", lineno, line.strip()))

        if "NODE_PKG" in enabled:
            m = _NODE_PKG_RE.search(line)
            if m:
                out.append(("NODE_PKG", lineno, m.group(0)))

        if ("QUICKJS_REQ" in enabled and in_quickjs
                and _REQUIRE_RE.search(line)
                and not _NO_REQUIRE_RE.search(line)):
            out.append(("QUICKJS_REQ", lineno, line.strip()[:70]))

        if ("CMAKE_FLAG" in enabled and in_fence
                and fence_lang in _SHELL_LANGS):
            m = _CMAKE_FLAG_RE.search(line)
            if m:
                out.append(("CMAKE_FLAG", lineno, m.group(0)))

    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Lint hand-written docs for wrong package names, "
                    "deprecated CMake flags, and emoji."
    )
    parser.add_argument("--quiet", action="store_true",
                        help="only print failures")
    parser.add_argument("--rules", default=",".join(RULES),
                        help=f"comma-separated subset of: {', '.join(RULES)}")
    args = parser.parse_args()

    enabled = {r.strip().upper() for r in args.rules.split(",") if r.strip()}
    unknown = enabled - set(RULES)
    if unknown:
        print(f"::error::unknown rule(s): {', '.join(sorted(unknown))}",
              file=sys.stderr)
        return 2

    pages = sorted(p for p in DOCS_DIR.rglob("*.md")
                   if p.name not in EXCLUDED_NAMES)

    failures: List[str] = []
    exempted: Set[Tuple[str, str]] = set()

    for page in pages:
        rel = page.relative_to(REPO_ROOT).as_posix()
        for rule, lineno, detail in _scan(page, rel, enabled):
            if (rel, rule) in _EXEMPTIONS:
                exempted.add((rel, rule))
                continue
            failures.append(
                f"::error file={rel},line={lineno}::[{rule}] {detail} — "
                f"{_FIX_HINT[rule]}"
            )

    stale = sorted(
        key for key in _EXEMPTIONS
        if key[1] in enabled and key not in exempted
        and (REPO_ROOT / key[0]).exists()
    )

    rc = 0
    if failures:
        rc = 1
        print(f"::error::{len(failures)} doc convention violation(s):",
              file=sys.stderr)
        for line in failures:
            print(line, file=sys.stderr)
        print("If a violation is genuinely intentional, add a (page, rule) "
              "entry to _EXEMPTIONS in scripts/check_doc_conventions.py "
              "with a reason.", file=sys.stderr)

    if stale and enabled == set(RULES):
        rc = 1
        print(f"::error::{len(stale)} stale exemption(s) in "
              f"scripts/check_doc_conventions.py — the violation is gone, "
              f"delete the entry:", file=sys.stderr)
        for rel, rule in stale:
            print(f"::error file=scripts/check_doc_conventions.py::"
                  f"({rel}, {rule}) no longer triggers", file=sys.stderr)

    if not args.quiet:
        print(f"Checked {len(pages)} doc pages for "
              f"{len(enabled)} convention rule(s); "
              f"{len(exempted)} exempted (page, rule) pair(s) still active.")
        if rc == 0:
            print("OK: no convention violations.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
