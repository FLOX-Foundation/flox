"""list_capi_functions — search the FLOX C-API surface from the ABI snapshot.

The ABI snapshot at `.api/c-api.snapshot` is the canonical pipe-delimited
serialization of every `flox_*` function. Bundled into the MCP package
via `flox_mcp/data/c-api.snapshot`; sync script keeps it current.
"""
from __future__ import annotations

from pathlib import Path
from typing import Optional


_DATA_PATH = Path(__file__).resolve().parent.parent / "data" / "c-api.snapshot"
_REPO_PATH = Path(__file__).resolve().parents[3] / ".api" / "c-api.snapshot"


def _resolve_path() -> Optional[Path]:
    if _DATA_PATH.exists():
        return _DATA_PATH
    if _REPO_PATH.exists():
        return _REPO_PATH
    return None


def _parse(text: str) -> list[tuple[str, str, list[str]]]:
    """Parse the snapshot into (name, return_type, [param_types])."""
    out: list[tuple[str, str, list[str]]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("|")
        if len(parts) < 2:
            continue
        out.append((parts[0], parts[1], parts[2:]))
    return out


def list_capi_functions(filter: Optional[str] = None, limit: int = 50) -> str:
    path = _resolve_path()
    if path is None:
        return (
            "C-API snapshot not found. The MCP package ships a copy in "
            "flox_mcp/data/; reinstalling should restore it."
        )

    fns = _parse(path.read_text())
    if filter:
        f = filter.lower()
        fns = [t for t in fns if f in t[0].lower()]

    total = len(fns)
    truncated = False
    if limit > 0 and len(fns) > limit:
        fns = fns[:limit]
        truncated = True

    lines = [
        f"# {total} C-API functions" + (f" matching {filter!r}" if filter else ""),
        "",
        "| Name | Signature |",
        "|---|---|",
    ]
    for name, ret, params in fns:
        sig = f"{ret} {name}(" + ", ".join(params) + ")"
        lines.append(f"| `{name}` | `{sig}` |")

    if truncated:
        lines.append("")
        lines.append(f"_Truncated to {limit} of {total} matches; "
                     f"narrow the filter or pass a higher `limit`._")

    return "\n".join(lines)
