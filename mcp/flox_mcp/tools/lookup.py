"""lookup_symbol + list_bindings — cross-language polyglot lookups.

Both tools are pure readers over the bundled artifacts produced by
``scripts/sync_mcp_data.py`` (``ir.snapshot.json`` +
``binding_manifest.json``). No live IDL parsing, no libclang —
everything ships as JSON with the wheel.
"""
from __future__ import annotations

import re
from typing import Any, Dict, Iterable, List, Optional, Tuple

from . import _data


VALID_LANGUAGES = ("capi", "python", "node", "codon", "quickjs")
_PYBIND_KEY = "pybind11"
_NAPI_KEY = "napi"


# ── Name canonicalization ─────────────────────────────────────────────


def _ir_name_to_capi_keys(name: str) -> List[str]:
    """Cross-language name → list of probable C-API symbols.

    Accepts any binding-local form (``Ema``, ``ema``, ``BarData``,
    ``FloxBarData``, ``flox_bar_data``) and returns plausible C-API
    matches. Order is preferred: most specific first.
    """
    out: List[str] = []
    n = name.strip()
    out.append(n)
    if not n.startswith("Flox") and re.fullmatch(r"[A-Z][A-Za-z0-9]+", n):
        out.append("Flox" + n)  # BarData → FloxBarData
    if not n.startswith("flox_") and re.fullmatch(r"[a-z][a-z_0-9]+", n):
        out.append(f"flox_indicator_{n}")  # ema → flox_indicator_ema
    return out


def _ir_function_index(ir: dict) -> Dict[str, dict]:
    return {fn["name"]: fn for fn in ir.get("functions", [])}


def _ir_struct_index(ir: dict) -> Dict[str, dict]:
    return {st["name"]: st for st in ir.get("structs", [])}


def _ir_enum_index(ir: dict) -> Dict[str, dict]:
    return {en["name"]: en for en in ir.get("enums", [])}


def _ir_handle_index(ir: dict) -> Dict[str, dict]:
    return {h["name"]: h for h in ir.get("typedefs", [])}


def _format_function_signature(fn: dict) -> str:
    params = ", ".join(f"{p['type']} {p['name']}".strip()
                       for p in fn.get("params", []))
    return f"{fn['return_type']} {fn['name']}({params})"


def _format_struct_signature(st: dict) -> str:
    fields = "; ".join(f"{f['type']} {f['name']}" for f in st["fields"])
    return f"struct {st['name']} {{ {fields} }}"


def _format_enum_signature(en: dict) -> str:
    values = ", ".join(v["name"] for v in en["values"])
    return f"enum {en['name']} {{ {values} }}"


# ── Per-binding name maps ─────────────────────────────────────────────


def _flatten_binding_symbols(manifest: dict, key: str) -> List[dict]:
    return list(manifest.get("bindings", {}).get(key, {}).get("symbols", []))


def _python_local_name_for(group: str, manifest: dict) -> Tuple[List[str], List[str]]:
    grp = manifest.get("groups", {}).get(group) or {}
    py = grp.get(_PYBIND_KEY) or {}
    return list(py.get("classes") or []), list(py.get("functions") or [])


def _node_local_name_for(group: str, manifest: dict) -> Tuple[List[str], List[str]]:
    grp = manifest.get("groups", {}).get(group) or {}
    nd = grp.get(_NAPI_KEY) or {}
    return list(nd.get("classes") or []), list(nd.get("functions") or [])


# ── lookup_symbol ─────────────────────────────────────────────────────


def _resolve_capi(ir: dict, name: str) -> Optional[dict]:
    """Match `name` against the IR. Tries function/struct/enum/handle in
    that order. Falls back to common naming conventions (``BarData`` →
    ``FloxBarData``, ``ema`` → ``flox_indicator_ema``)."""
    candidates = _ir_name_to_capi_keys(name)
    fns = _ir_function_index(ir)
    structs = _ir_struct_index(ir)
    enums = _ir_enum_index(ir)
    handles = _ir_handle_index(ir)

    for c in candidates:
        if c in fns:
            return {"kind": "function", "name": c,
                    "signature": _format_function_signature(fns[c]),
                    "group": fns[c].get("group", "_ungrouped")}
        if c in structs:
            return {"kind": "struct", "name": c,
                    "signature": _format_struct_signature(structs[c])}
        if c in enums:
            return {"kind": "enum", "name": c,
                    "signature": _format_enum_signature(enums[c])}
        if c in handles:
            target = handles[c].get("target") or handles[c].get("alias_of")
            return {"kind": "handle", "name": c,
                    "signature": f"typedef {target} {c}"}
    return None


def _resolve_python(name: str, manifest: dict, ir_match: Optional[dict]) -> Optional[dict]:
    sym = {s["name"] for s in _flatten_binding_symbols(manifest, "pybind11")}
    if name in sym:
        return {"name": name, "kind": "class" if name[0:1].isupper() else "function"}
    if ir_match is not None and ir_match.get("group"):
        classes, fns = _python_local_name_for(ir_match["group"], manifest)
        for c in classes:
            return {"name": c, "kind": "class", "from_group": ir_match["group"]}
        for f in fns:
            return {"name": f, "kind": "function", "from_group": ir_match["group"]}
    return None


def _resolve_node(name: str, manifest: dict, ir_match: Optional[dict]) -> Optional[dict]:
    sym = {s["name"] for s in _flatten_binding_symbols(manifest, "napi")}
    if name in sym:
        return {"name": name, "kind": "class" if name[0:1].isupper() else "function"}
    if ir_match is not None and ir_match.get("group"):
        classes, fns = _node_local_name_for(ir_match["group"], manifest)
        for c in classes:
            return {"name": c, "kind": "class", "from_group": ir_match["group"]}
        for f in fns:
            return {"name": f, "kind": "function", "from_group": ir_match["group"]}
    return None


def _resolve_codon(name: str, manifest: dict, ir_match: Optional[dict]) -> Optional[dict]:
    sym = {s["name"] for s in _flatten_binding_symbols(manifest, "codon")}
    if name in sym:
        return {"name": name, "kind": "function"}
    if ir_match is not None and ir_match.get("name") in sym:
        return {"name": ir_match["name"], "kind": "function"}
    return None


def lookup_symbol(name: str, language: Optional[str] = None) -> str:
    """Resolve a symbol across bindings.

    Returns Markdown — a small table per binding that exports the
    symbol. Unknown returns "no match" and a hint to call
    ``list_bindings`` for browsing.
    """
    if not name or not isinstance(name, str):
        return "lookup_symbol: `name` is required and must be a string."
    if language is not None and language not in VALID_LANGUAGES:
        return (
            f"lookup_symbol: unknown language {language!r}. "
            f"Valid: {', '.join(VALID_LANGUAGES)}."
        )

    ir = _data.load_ir_snapshot()
    manifest = _data.load_binding_manifest()
    if ir is None or manifest is None:
        return ("lookup_symbol: bundled IR / manifest data missing. "
                "Reinstall flox-mcp; the wheel ships the snapshots.")

    capi_match = _resolve_capi(ir, name)
    py_match = _resolve_python(name, manifest, capi_match)
    nd_match = _resolve_node(name, manifest, capi_match)
    cd_match = _resolve_codon(name, manifest, capi_match)
    qj_match = None  # quickjs binding does not yet contribute symbols

    rows: List[Tuple[str, dict]] = []
    if language is None or language == "capi":
        if capi_match is not None:
            rows.append(("capi", capi_match))
    if language is None or language == "python":
        if py_match is not None:
            rows.append(("python", py_match))
    if language is None or language == "node":
        if nd_match is not None:
            rows.append(("node", nd_match))
    if language is None or language == "codon":
        if cd_match is not None:
            rows.append(("codon", cd_match))
    if language is None or language == "quickjs":
        if qj_match is not None:
            rows.append(("quickjs", qj_match))

    if not rows:
        return (
            f"# lookup_symbol: no match for `{name}`\n\n"
            f"Tried C-API, Python, Node, Codon"
            f"{' (filtered to ' + language + ')' if language else ''}."
            f" Use `list_bindings(language=...)` to browse the surface."
        )

    out = [f"# lookup_symbol: `{name}`", ""]
    out.append("| Binding | Local name | Kind | Signature |")
    out.append("|---|---|---|---|")
    for binding, m in rows:
        sig = m.get("signature", "")
        out.append(
            f"| {binding} | `{m.get('name', '')}` | {m.get('kind', '')} | "
            f"`{sig}` |" if sig else
            f"| {binding} | `{m.get('name', '')}` | {m.get('kind', '')} |  |"
        )
    return "\n".join(out)


# ── list_bindings ─────────────────────────────────────────────────────


def list_bindings(language: str, filter: Optional[str] = None,
                  limit: int = 50) -> str:
    """Enumerate the exports of one binding surface."""
    if language not in VALID_LANGUAGES:
        return (
            f"list_bindings: unknown language {language!r}. "
            f"Valid: {', '.join(VALID_LANGUAGES)}."
        )
    if not isinstance(limit, int) or limit <= 0:
        return "list_bindings: `limit` must be a positive integer."

    manifest = _data.load_binding_manifest()
    if manifest is None:
        return ("list_bindings: bundled binding_manifest.json missing. "
                "Reinstall flox-mcp.")

    binding_key = {"python": "pybind11", "node": "napi"}.get(language, language)
    symbols = list(_flatten_binding_symbols(manifest, binding_key))

    if filter:
        f = filter.lower()
        symbols = [s for s in symbols if f in s.get("name", "").lower()]

    total = len(symbols)
    truncated = False
    if total > limit:
        symbols = symbols[:limit]
        truncated = True

    if total == 0:
        if language == "quickjs":
            return (
                f"# {language} binding\n\n"
                "No QuickJS exports recorded yet. The QuickJS binding is "
                "experimental and currently surfaced through the C ABI "
                "directly; the surface inventory will land once the "
                "binding stabilises."
            )
        return (
            f"# {language} binding\n\nNo symbols match"
            + (f" filter {filter!r}" if filter else "")
            + ". Call without a filter to browse the full surface."
        )

    lines = [f"# {language} binding ({total} symbol(s)"
             + (f" matching {filter!r}" if filter else "")
             + ")", ""]
    lines.append("| Name | Kind | Group / Notes |")
    lines.append("|---|---|---|")
    for s in symbols:
        notes = s.get("group") or s.get("from_group") or ""
        lines.append(
            f"| `{s.get('name', '')}` | {s.get('kind', '')} | "
            f"{notes} |"
        )

    if truncated:
        lines.append("")
        lines.append(
            f"_Truncated to {limit} of {total}; pass a higher `limit` "
            f"or use `filter` to narrow._"
        )
    return "\n".join(lines)
