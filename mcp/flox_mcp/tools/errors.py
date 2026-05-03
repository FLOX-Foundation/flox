"""lookup_error_code — return the bundled Markdown page for an error code.

The canonical source of truth is `docs/errors/<code>.md` in the FLOX
repo; the MCP package bundles a copy in `flox_mcp/data/errors/` so the
tool works without a repo checkout. `scripts/sync_mcp_data.py` keeps
the bundled copies in sync, gated by CI.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Optional


_DATA_DIR = Path(__file__).resolve().parent.parent / "data" / "errors"
# Editable / repo-checkout fallback when the bundled copy is missing.
_REPO_DIR = Path(__file__).resolve().parents[3] / "docs" / "errors"

_CODE_RE = re.compile(r"^E_[A-Z]+_\d{3,4}$")


def _resolve_dir() -> Optional[Path]:
    if _DATA_DIR.is_dir() and any(_DATA_DIR.glob("E_*.md")):
        return _DATA_DIR
    if _REPO_DIR.is_dir() and any(_REPO_DIR.glob("E_*.md")):
        return _REPO_DIR
    return None


def lookup_error_code(code: str) -> str:
    if not _CODE_RE.match(code):
        return (
            f"Invalid error code format: {code!r}. Expected E_<DOMAIN>_<NNN>, "
            "e.g. E_SYM_001."
        )

    src = _resolve_dir()
    if src is None:
        return (
            "flox-mcp can't find the error catalog. Either reinstall the "
            "package (data/errors/ is missing) or run from a FLOX repo "
            "checkout."
        )

    page = src / f"{code}.md"
    if not page.exists():
        listed = sorted(p.stem for p in src.glob("E_*.md"))
        if not listed:
            return f"Error catalog at {src} is empty."
        return (
            f"No page for {code}. Known codes:\n  - " + "\n  - ".join(listed)
        )
    return page.read_text()
