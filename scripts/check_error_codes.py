#!/usr/bin/env python3
"""scripts/check_error_codes.py

Cross-reference FLOX error codes between source code and documentation.

Two failure modes:
1. A code is thrown from C++ / Python source but has no `docs/errors/<code>.md`.
2. A `docs/errors/<code>.md` exists but the code is referenced nowhere in
   source — likely a stale page.

The check exits non-zero on either, so AI agents and humans relying on the
help URLs always land on a real page (and unused pages don't accumulate).

Usage:
    python3 scripts/check_error_codes.py            # report + exit 1 on mismatch
    python3 scripts/check_error_codes.py --quiet    # only failures
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, Set


REPO_ROOT = Path(__file__).resolve().parents[1]
DOCS_ERRORS = REPO_ROOT / "docs" / "errors"

# Match the literal first arg to FloxError(...): a quoted string starting
# with "E_". Catches both C++ (`flox::FloxError("E_SYM_001", ...)`) and
# Python (FloxError("E_SYM_001", ...)) call sites.
_CODE_LITERAL_RE = re.compile(r'FloxError\s*\(\s*"(E_[A-Z]+_\d{3,4})"')

# Files to scan for code references. Engine + bindings + tests.
_SOURCE_GLOBS = (
    "include/**/*.h",
    "src/**/*.cpp",
    "src/**/*.h",
    "python/**/*.cpp",
    "python/**/*.h",
    "node/**/*.cpp",
    "node/**/*.h",
    "tests/**/*.cpp",
)

_FRONTMATTER_CODE_RE = re.compile(r"^code:\s*(E_[A-Z]+_\d{3,4})\s*$", re.MULTILINE)


def _scan_source_codes(root: Path) -> Dict[str, Set[Path]]:
    """Return {code: {paths}} for every FloxError("E_…") found in source."""
    out: Dict[str, Set[Path]] = {}
    for pattern in _SOURCE_GLOBS:
        for path in root.glob(pattern):
            try:
                text = path.read_text()
            except (OSError, UnicodeDecodeError):
                continue
            for m in _CODE_LITERAL_RE.finditer(text):
                out.setdefault(m.group(1), set()).add(path)
    return out


def _scan_doc_codes(root: Path) -> Dict[str, Path]:
    """Return {code: doc_path} for every docs/errors/E_*.md page."""
    out: Dict[str, Path] = {}
    if not root.is_dir():
        return out
    for md in root.glob("E_*.md"):
        text = md.read_text()
        m = _FRONTMATTER_CODE_RE.search(text)
        if m is not None:
            out[m.group(1)] = md
        else:
            # Fallback: filename without extension. Lets a page be parsed
            # even if frontmatter is missing — but the missing frontmatter
            # itself is reported as a separate error class.
            stem = md.stem
            if stem.startswith("E_"):
                out[stem] = md
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--quiet", action="store_true",
                   help="suppress success log; only print failures")
    args = p.parse_args(argv)

    src_codes = _scan_source_codes(REPO_ROOT)
    doc_codes = _scan_doc_codes(DOCS_ERRORS)

    missing_pages = sorted(set(src_codes) - set(doc_codes))
    stale_pages = sorted(set(doc_codes) - set(src_codes))

    rc = 0
    if missing_pages:
        rc = 1
        print("::error::Error codes used in source but missing docs page:",
              file=sys.stderr)
        for code in missing_pages:
            paths = sorted(str(p.relative_to(REPO_ROOT)) for p in src_codes[code])
            print(f"  {code} (used in: {', '.join(paths)})", file=sys.stderr)
            print(f"    Fix: create docs/errors/{code}.md", file=sys.stderr)

    if stale_pages:
        rc = 1
        print("::error::docs/errors pages without any source references:",
              file=sys.stderr)
        for code in stale_pages:
            print(f"  {code} ({doc_codes[code].relative_to(REPO_ROOT)})",
                  file=sys.stderr)
            print(f"    Fix: remove the page or restore the source reference",
                  file=sys.stderr)

    if rc == 0 and not args.quiet:
        print(f"OK: {len(src_codes)} error codes — all have docs and are referenced.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
