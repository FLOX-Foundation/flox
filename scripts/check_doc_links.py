#!/usr/bin/env python3
"""scripts/check_doc_links.py

Validate intra-repo Markdown links across `docs/**/*.md`:

1. `[text](some/page.md)` — the target file must exist.
2. `[text](some/page.md#anchor)` — the anchor must exist in that page.
3. `[text](#anchor)` — the anchor must exist in the current page.
4. `[text](../../src/foo.cpp)` — a repo-relative link out of `docs/` must
   point at a path that exists in the working tree.

mkdocs only *warns* on a dead relative link, so these rot silently and the
site ships 404s.

Anchor ids are reproduced the way python-markdown's `toc` extension builds
them (NFKD, strip non-word, lowercase, spaces to `-`, `_N` suffix on
duplicates), plus explicit ids from `attr_list` (`## Title {#custom}`) and
raw HTML (`<a id="x">` / `<a name="x">`).

False-positive guard: C++ lambdas (`[&](const Order& o)`) and array
indexing in prose look exactly like Markdown links. A target therefore only
counts as a link when it looks like a path — it contains `.md`, contains
`/`, or starts with `#`.

Usage:
    python3 scripts/check_doc_links.py            # report + exit 1 on failure
    python3 scripts/check_doc_links.py --quiet     # only failures
"""
from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from pathlib import Path
from typing import Dict, List, Set, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = REPO_ROOT / "docs"

# Generated aggregates: not hand-written, and they inline every other page.
EXCLUDED = {"llms.txt", "llms-full.txt"}

_FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")
_LINK_RE = re.compile(r"\[(?P<text>[^\]\[]*)\]\((?P<target>[^()\s]+)\)")
_HEADING_RE = re.compile(r"^(#{1,6})\s+(?P<title>.*?)\s*$")
_ATTR_ID_RE = re.compile(r"\{:?\s*#(?P<id>[^\s}]+)[^}]*\}\s*$")
_HTML_ID_RE = re.compile(r"<a\s[^>]*(?:id|name)=[\"'](?P<id>[^\"']+)[\"']")
_INLINE_CODE_RE = re.compile(r"`[^`]*`")
_HTML_TAG_RE = re.compile(r"<[^>]+>")

_EXTERNAL_PREFIXES = ("http://", "https://", "mailto:", "tel:", "ftp://",
                      "data:", "//")


def _slugify(text: str) -> str:
    """Reproduce python-markdown's toc slugify(text, '-')."""
    value = unicodedata.normalize("NFKD", text)
    value = value.encode("ascii", "ignore").decode("ascii")
    value = re.sub(r"[^\w\s-]", "", value).strip().lower()
    return re.sub(r"[-\s]+", "-", value)


def _unique(slug: str, seen: Set[str]) -> str:
    """Reproduce toc's duplicate-id disambiguation (`slug`, `slug_1`, ...)."""
    candidate = slug
    counter = 0
    while candidate in seen or not candidate:
        counter += 1
        candidate = f"{slug}_{counter}"
    seen.add(candidate)
    return candidate


def _visible_heading_text(raw: str) -> str:
    """Strip the markup toc sees stripped: backticks, links, html, bold."""
    text = raw
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", text)   # images
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)    # links
    text = _HTML_TAG_RE.sub("", text)
    text = text.replace("`", "")
    # Only `*` emphasis is stripped: python-markdown leaves intra-word `_`
    # alone, so `## order_type encodings` keeps its underscore in the id.
    text = re.sub(r"\*{1,3}", "", text)
    return text.strip()


def _collect_anchors(path: Path) -> Set[str]:
    """Every id a link can target inside one page."""
    anchors: Set[str] = set()
    seen: Set[str] = set()
    in_fence = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if _FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for m in _HTML_ID_RE.finditer(line):
            anchors.add(m.group("id"))
        heading = _HEADING_RE.match(line)
        if not heading:
            continue
        title = heading.group("title")
        explicit = _ATTR_ID_RE.search(title)
        if explicit:
            anchors.add(explicit.group("id"))
            seen.add(explicit.group("id"))
            continue
        anchors.add(_unique(_slugify(_visible_heading_text(title)), seen))
    return anchors


def _iter_links(path: Path) -> List[Tuple[int, str]]:
    """Yield (line_no, target) for every path-looking inline link."""
    out: List[Tuple[int, str]] = []
    in_fence = False
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if _FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        stripped = _INLINE_CODE_RE.sub("``", line)
        for m in _LINK_RE.finditer(stripped):
            target = m.group("target").strip()
            if not target or target.startswith(_EXTERNAL_PREFIXES):
                continue
            # Path-looking only: kills `[&](const Order& o)` and `[0](i)`.
            if not (target.startswith("#") or ".md" in target or "/" in target):
                continue
            out.append((lineno, target))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate relative .md links and #anchor fragments in "
                    "docs/**/*.md."
    )
    parser.add_argument("--quiet", action="store_true",
                        help="only print failures")
    args = parser.parse_args()

    pages = sorted(p for p in DOCS_DIR.rglob("*.md") if p.name not in EXCLUDED)
    anchor_cache: Dict[Path, Set[str]] = {}

    def anchors_of(path: Path) -> Set[str]:
        if path not in anchor_cache:
            anchor_cache[path] = _collect_anchors(path)
        return anchor_cache[path]

    failures: List[str] = []
    checked = 0

    for page in pages:
        for lineno, target in _iter_links(page):
            checked += 1
            rel = page.relative_to(REPO_ROOT)

            if target.startswith("#"):
                anchor = target[1:]
                if anchor and anchor not in anchors_of(page):
                    failures.append(
                        f"::error file={rel},line={lineno}::dead anchor "
                        f"`{target}` — no heading in this page produces that "
                        f"id"
                    )
                continue

            path_part, _, anchor = target.partition("#")
            if not path_part:
                continue

            if path_part.startswith("/"):
                # Site-absolute: resolve against docs/.
                resolved = (DOCS_DIR / path_part.lstrip("/")).resolve()
            else:
                resolved = (page.parent / path_part).resolve()

            if not resolved.exists():
                failures.append(
                    f"::error file={rel},line={lineno}::dead link "
                    f"`{target}` — resolves to "
                    f"{_relative(resolved)} which does not exist"
                )
                continue

            if anchor and resolved.suffix == ".md":
                if anchor not in anchors_of(resolved):
                    failures.append(
                        f"::error file={rel},line={lineno}::dead anchor "
                        f"`{target}` — {_relative(resolved)} has no id "
                        f"`{anchor}`"
                    )

    rc = 0
    if failures:
        rc = 1
        print(f"::error::{len(failures)} broken doc link(s):", file=sys.stderr)
        for line in failures:
            print(line, file=sys.stderr)

    if not args.quiet:
        print(f"Checked {checked} intra-repo links across {len(pages)} pages.")
        if rc == 0:
            print("OK: every relative link and anchor resolves.")
    return rc


def _relative(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


if __name__ == "__main__":
    sys.exit(main())
