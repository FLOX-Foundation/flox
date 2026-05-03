"""list_indicators — introspect flox_py for streaming + batch indicators.

Reads the .pyi stubs that ship with the binding (no need to instantiate
the actual extension at MCP startup) so the tool works even when the
compiled binding isn't installed in the same env. If the binding is
present, falls back to runtime introspection for richer metadata.
"""
from __future__ import annotations

import ast
from pathlib import Path
from typing import Optional

# Indicator class names ship in the binding's __all__; we don't enumerate
# them here. Heuristics: a streaming class has both update() and value;
# a batch counterpart is the same name lowercased (sma → SMA).

_PYI_CANDIDATES = [
    # Editable install: the binding sits next to this MCP package.
    Path(__file__).resolve().parents[3] / "python" / "flox_py" / "_flox_py" / "__init__.pyi",
    # Wheel install: rely on the installed flox_py package.
    Path("python") / "flox_py" / "_flox_py" / "__init__.pyi",
]


def _find_pyi() -> Optional[Path]:
    for p in _PYI_CANDIDATES:
        if p.exists():
            return p
    # Last resort: import flox_py and locate its stubs.
    try:
        import flox_py  # type: ignore
        pkg_dir = Path(flox_py.__file__).resolve().parent
        candidate = pkg_dir / "_flox_py" / "__init__.pyi"
        if candidate.exists():
            return candidate
    except Exception:
        pass
    return None


def _is_streaming_class(cls: ast.ClassDef) -> bool:
    """Streaming indicator classes have update() and either value/property."""
    method_names = {b.name for b in cls.body if isinstance(b, ast.FunctionDef)}
    has_update = "update" in method_names
    has_value = "value" in method_names  # property
    return has_update and has_value


def _format_init(cls: ast.ClassDef) -> str:
    for b in cls.body:
        if isinstance(b, ast.FunctionDef) and b.name == "__init__":
            args = b.args
            parts: list[str] = []
            pos = list(args.args)
            defaults = list(args.defaults)
            n_no = len(pos) - len(defaults)
            for i, a in enumerate(pos[1:], start=1):  # skip self
                d = defaults[i - 1 - (n_no - 1)] if i - 1 >= n_no - 1 else None
                if d is None and i - 1 < n_no:
                    d_str = ""
                else:
                    try:
                        d_str = " = " + ast.unparse(d) if d is not None else ""
                    except Exception:
                        d_str = ""
                ann = ": " + ast.unparse(a.annotation) if a.annotation else ""
                parts.append(f"{a.arg}{ann}{d_str}")
            return f"({', '.join(parts)})"
    return "()"


def _scan(pyi_path: Path) -> list[dict]:
    tree = ast.parse(pyi_path.read_text())
    out: list[dict] = []
    func_names = {b.name for b in tree.body if isinstance(b, ast.FunctionDef)}

    for node in tree.body:
        if not isinstance(node, ast.ClassDef):
            continue
        if not _is_streaming_class(node):
            continue
        # Batch counterpart: lowercased class name as a free function.
        batch_name = node.name.lower()
        out.append({
            "class": node.name,
            "constructor": _format_init(node),
            "batch_function": batch_name if batch_name in func_names else None,
            "shape": _shape_hint(node),
        })
    out.sort(key=lambda d: d["class"].lower())
    return out


def _shape_hint(cls: ast.ClassDef) -> str:
    """Heuristic shape detection from update() signature."""
    for b in cls.body:
        if isinstance(b, ast.FunctionDef) and b.name == "update":
            n = len(b.args.args) - 1  # minus self
            if n == 1:
                return "single_input"
            if n == 3:
                return "ohlc_or_hlc"
            if n >= 4:
                return "multi_input"
            return "unknown"
    return "unknown"


def list_indicators(filter: Optional[str] = None) -> str:
    pyi_path = _find_pyi()
    if pyi_path is None:
        return (
            "Could not locate flox_py .pyi stubs. The MCP server reads them\n"
            "to enumerate indicators without importing the binding. Either\n"
            "install flox-py in the same env (`pip install flox-py`) or run\n"
            "the MCP server from inside the FLOX repo checkout."
        )
    rows = _scan(pyi_path)
    if filter is not None:
        f = filter.lower()
        rows = [r for r in rows if f in r["class"].lower()
                or (r["batch_function"] and f in r["batch_function"])]

    if not rows:
        return f"No indicators match filter {filter!r}."

    lines = [
        f"# {len(rows)} indicators in flox_py",
        "",
        "| Class | Constructor | Batch fn | Shape |",
        "|---|---|---|---|",
    ]
    for r in rows:
        batch = r["batch_function"] or "—"
        lines.append(f"| `{r['class']}` | `{r['class']}{r['constructor']}` "
                     f"| `{batch}` | {r['shape']} |")
    return "\n".join(lines)
