#!/usr/bin/env python3
"""scripts/check_doc_nav.py

Verify that `mkdocs.yml` nav and `docs/**/*.md` are the same set of pages.

Two failure modes:

1. **Orphan page** — a Markdown file under `docs/` that no nav entry points
   at. The theme enables `navigation.prune`, so a page absent from nav is
   built but unreachable: no sidebar entry, no breadcrumb, no "next" link.
   Only site search or a direct URL finds it.
2. **Dangling nav entry** — a nav entry pointing at a file that does not
   exist. mkdocs only warns about these by default, so they survive PRs.

`mkdocs.yml` uses a `!!python/name:` tag for the mermaid custom fence, which
`yaml.safe_load` refuses. The loader below tolerates unknown tags (they are
irrelevant to nav) and there is a regex fallback if PyYAML is missing.

Usage:
    python3 scripts/check_doc_nav.py            # report + exit 1 on mismatch
    python3 scripts/check_doc_nav.py --quiet    # only failures
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable, List, Set


REPO_ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = REPO_ROOT / "docs"
MKDOCS_YML = REPO_ROOT / "mkdocs.yml"

_MD_IN_NAV_RE = re.compile(r"([A-Za-z0-9_./-]+\.md)")


def _walk_nav(node: object) -> Iterable[str]:
    """Yield every string target found in a parsed mkdocs nav tree."""
    if isinstance(node, str):
        yield node
    elif isinstance(node, list):
        for item in node:
            yield from _walk_nav(item)
    elif isinstance(node, dict):
        for value in node.values():
            yield from _walk_nav(value)


def _nav_targets_via_yaml(text: str) -> List[str] | None:
    """Parse nav with a tag-tolerant loader. None if PyYAML is unavailable."""
    try:
        import yaml
    except ImportError:
        return None

    class _TagTolerant(yaml.SafeLoader):
        pass

    # `!!python/name:pymdownx...` and any other tag becomes None: nav never
    # uses tags, so dropping them loses nothing this gate cares about.
    _TagTolerant.add_multi_constructor(
        "", lambda loader, suffix, node: None
    )
    _TagTolerant.add_multi_constructor(
        "tag:yaml.org,2002:python/name", lambda loader, suffix, node: None
    )
    config = yaml.load(text, Loader=_TagTolerant)
    if not isinstance(config, dict) or "nav" not in config:
        return None
    return [t for t in _walk_nav(config["nav"]) if isinstance(t, str)]


def _nav_targets_via_regex(text: str) -> List[str]:
    """Fallback: pull `*.md` paths out of the `nav:` block by hand."""
    lines = text.splitlines()
    try:
        start = next(i for i, ln in enumerate(lines) if ln.rstrip() == "nav:")
    except StopIteration:
        return []
    out: List[str] = []
    for line in lines[start + 1:]:
        if line and not line[0].isspace():
            break  # dedented back to a top-level key: nav block is over
        out.extend(_MD_IN_NAV_RE.findall(line))
    return out


def _docs_pages() -> Set[str]:
    return {
        str(p.relative_to(DOCS_DIR).as_posix())
        for p in DOCS_DIR.rglob("*.md")
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check that mkdocs.yml nav covers every docs/**/*.md "
                    "and points only at files that exist."
    )
    parser.add_argument("--quiet", action="store_true",
                        help="only print failures")
    args = parser.parse_args()

    if not MKDOCS_YML.exists():
        print(f"::error::{MKDOCS_YML} not found", file=sys.stderr)
        return 1

    text = MKDOCS_YML.read_text(encoding="utf-8")
    targets = _nav_targets_via_yaml(text)
    used_fallback = targets is None
    if targets is None:
        targets = _nav_targets_via_regex(text)

    nav_pages = {t for t in targets if t.endswith(".md")}
    docs_pages = _docs_pages()

    orphans = sorted(docs_pages - nav_pages)
    dangling = sorted(nav_pages - docs_pages)

    rc = 0
    if orphans:
        rc = 1
        print(f"::error::{len(orphans)} doc page(s) are not in the mkdocs.yml "
              f"nav. `navigation.prune` is on, so they are unreachable from "
              f"the site navigation:", file=sys.stderr)
        for page in orphans:
            print(f"::error file=docs/{page}::not referenced by mkdocs.yml "
                  f"nav — add an entry or delete the page", file=sys.stderr)

    if dangling:
        rc = 1
        print(f"::error::{len(dangling)} nav entr(ies) point at files that do "
              f"not exist:", file=sys.stderr)
        for page in dangling:
            print(f"::error file=mkdocs.yml::nav references missing page "
                  f"docs/{page}", file=sys.stderr)

    if not args.quiet:
        via = "regex fallback (PyYAML missing)" if used_fallback else "PyYAML"
        print(f"Parsed mkdocs.yml nav via {via}: {len(nav_pages)} page "
              f"entries; docs/ holds {len(docs_pages)} Markdown files.")
        if rc == 0:
            print("OK: nav and docs/ are in sync.")

    return rc


if __name__ == "__main__":
    sys.exit(main())
