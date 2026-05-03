#!/usr/bin/env python3
"""scripts/check_doc_snippets.py

Lint Markdown docs for runnable code blocks that aren't backed by an
executable file under `docs/examples/`.

The cleanest way to keep doc snippets correct is the **`--8<--` snippet
inclusion** pattern: write the snippet once as a real `.py`/`.js`/`.codon`
file under `docs/examples/`, run it in CI, and reference it from Markdown
via:

    ```python
    --8<-- "examples/quickstart.py"
    ```

This script enforces that pattern for *new* docs while grandfathering
existing inline snippets via `docs/.snippet-allowlist.txt`. The
allowlist is human-curated; entries that should be migrated are tracked
there with a one-line rationale.

Failure modes:

1. A doc file contains an inline (non-`--8<--`) code block in a typed
   language and the file is NOT in the allowlist → fail.
2. A doc file is in the allowlist but contains zero typed code blocks
   anymore → fail (stale entry; remove it from the allowlist).

Lints languages: python, py, js, javascript, typescript, ts, node, codon.

Usage:
    python3 scripts/check_doc_snippets.py            # report + exit 1 on violation
    python3 scripts/check_doc_snippets.py --quiet    # only failures
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Set


REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS_DIR = REPO_ROOT / "docs"
ALLOWLIST_PATH = DOCS_DIR / ".snippet-allowlist.txt"

LINTED_LANGS = {
    "python", "py",
    "js", "javascript", "node",
    "typescript", "ts",
    "codon",
}

# Match a fenced code block opener: ```lang [...]
_FENCE_RE = re.compile(r"^```([A-Za-z0-9_+-]+)")
_INCLUDE_RE = re.compile(r"^\s*--8<--\s+\".+?\"\s*$")


def _load_allowlist(path: Path) -> Dict[str, str]:
    """Read allowlist file. Each non-comment line is `path # rationale`."""
    out: Dict[str, str] = {}
    if not path.exists():
        return out
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "#" in line:
            file_part, rationale = line.split("#", 1)
        else:
            file_part, rationale = line, ""
        out[file_part.strip()] = rationale.strip()
    return out


def _scan_blocks(md_path: Path) -> tuple[int, int]:
    """Return (typed_blocks_total, blocks_via_include) for a Markdown file.

    A block "via include" has its first non-blank inner line matching the
    `--8<--` directive. Blocks with no body or with code outside that
    pattern are inline.
    """
    text = md_path.read_text()
    lines = text.splitlines()
    total = 0
    via_include = 0

    in_block = False
    block_lang = ""
    block_lines: List[str] = []

    for line in lines:
        if not in_block:
            m = _FENCE_RE.match(line)
            if m and m.group(1).lower() in LINTED_LANGS:
                in_block = True
                block_lang = m.group(1).lower()
                block_lines = []
            continue
        # in_block
        if line.startswith("```"):
            in_block = False
            total += 1
            non_blank = [ln for ln in block_lines if ln.strip()]
            if non_blank and _INCLUDE_RE.match(non_blank[0]):
                via_include += 1
            block_lines = []
        else:
            block_lines.append(line)

    return total, via_include


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--quiet", action="store_true",
                   help="suppress per-file stats; only print failures")
    args = p.parse_args(argv)

    allowlist = _load_allowlist(ALLOWLIST_PATH)
    allowlisted_set: Set[str] = set(allowlist.keys())

    inline_violations: List[tuple[str, int]] = []  # (path, inline_count)
    stale_allowlist: List[str] = []
    total_blocks = 0
    total_via_include = 0
    files_with_blocks: List[tuple[str, int, int]] = []

    for md in sorted(DOCS_DIR.rglob("*.md")):
        rel = str(md.relative_to(REPO_ROOT))
        total, via = _scan_blocks(md)
        if total == 0:
            if rel in allowlisted_set:
                stale_allowlist.append(rel)
            continue

        files_with_blocks.append((rel, total, via))
        total_blocks += total
        total_via_include += via

        inline_count = total - via
        if inline_count > 0 and rel not in allowlisted_set:
            inline_violations.append((rel, inline_count))

    rc = 0
    if inline_violations:
        rc = 1
        print("::error::Inline (non-`--8<--`) snippets in docs without an "
              "allowlist entry:", file=sys.stderr)
        for path, count in inline_violations:
            print(f"  {path}: {count} inline block(s)", file=sys.stderr)
        print("", file=sys.stderr)
        print("Fix: extract the snippet to docs/examples/<name>.<ext>, "
              "wire it into CI, and replace the inline block with:",
              file=sys.stderr)
        print('    ```python', file=sys.stderr)
        print('    --8<-- "examples/<name>.<ext>"', file=sys.stderr)
        print('    ```', file=sys.stderr)
        print("Or, if the snippet is genuinely abstract (pseudocode), add "
              "the file to docs/.snippet-allowlist.txt with a rationale.",
              file=sys.stderr)

    if stale_allowlist:
        rc = 1
        print("::error::Allowlisted docs no longer have any typed code "
              "blocks (entries should be removed):", file=sys.stderr)
        for path in stale_allowlist:
            print(f"  {path}", file=sys.stderr)

    if not args.quiet:
        print(
            f"Scanned {len(files_with_blocks)} doc files: "
            f"{total_blocks} typed code blocks, "
            f"{total_via_include} via --8<-- include "
            f"({total_via_include * 100 // total_blocks if total_blocks else 0}% tested)."
        )
        if rc == 0:
            print("OK: no inline-snippet violations.")

    return rc


if __name__ == "__main__":
    sys.exit(main())
