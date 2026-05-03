"""IR → llms.txt-style C-API summary for AI agents.

Produces a Markdown file summarizing the FLOX C API surface: handles,
enums, structs, callback typedefs, and functions grouped by their `group=`
annotation. Keeps the doc small enough to fit a context window and machine-
readable enough that an LLM can ground answers about the C API without
parsing the actual header.

Lives next to the existing `scripts/gen_llms_txt.py` (which curates the
broader Markdown docs); this emitter focuses specifically on the C API
surface so AI agents writing code against the C ABI can be authoritative.
"""
from __future__ import annotations

from io import StringIO
from typing import List

from . import ir


def _format_function_signature(fn: ir.Function) -> str:
    """Render `int flox_x(double y, size_t n)` from an IR Function."""
    if fn.params:
        param_strs = [
            (f"{p.type} {p.name}".rstrip() if p.name else p.type)
            for p in fn.params
        ]
    else:
        param_strs = ["void"]
    return f"{fn.return_type} {fn.name}({', '.join(param_strs)})"


def emit(module: ir.Module) -> str:
    """Render the IR as a Markdown summary."""
    out = StringIO()
    out.write("# FLOX C API Reference\n\n")
    out.write(
        "Generated from `include/flox/capi/flox_capi_spec.hpp`. "
        "Source of truth for FFI consumers (Codon, QuickJS, Rust, Go cgo, "
        "Python ctypes). The pybind11 (Python) and NAPI (Node) bindings "
        "wrap this surface but expose richer language-native APIs that "
        "live in `python/` and `node/` respectively — see those for the "
        "Python/TS-flavored interfaces.\n\n"
    )

    grouped = module.functions_by_group()
    out.write(f"**Surface:** {len(module.functions)} functions, "
              f"{len(module.handles)} handles, {len(module.structs)} structs, "
              f"{len(module.function_pointers)} callback typedefs, "
              f"{len(module.enums)} enums, "
              f"{len(grouped)} groups.\n\n")

    if module.handles:
        out.write("## Opaque handles\n\n")
        out.write("All handles are typedef'd `void*`. Treat them as opaque; "
                  "manage lifetime via the matching `_create` / `_destroy` "
                  "pair.\n\n")
        for h in module.handles:
            if h.alias_of is None:
                out.write(f"- `{h.name}`\n")
            else:
                out.write(f"- `{h.name}` (alias of `{h.alias_of}`)\n")
        out.write("\n")

    if module.enums:
        out.write("## Enums\n\n")
        for enum in module.enums:
            out.write(f"### `{enum.name}`\n\n")
            for v in enum.values:
                if v.value is not None:
                    out.write(f"- `{v.name}` = `{v.value}`\n")
                else:
                    out.write(f"- `{v.name}`\n")
            out.write("\n")

    if module.function_pointers:
        out.write("## Callback typedefs\n\n")
        for fp in module.function_pointers:
            params = ", ".join(p.type for p in fp.params) or "void"
            out.write(f"- `typedef {fp.return_type} (*{fp.name})({params});`\n")
        out.write("\n")

    if module.structs:
        out.write("## Structs\n\n")
        for s in module.structs:
            out.write(f"### `{s.name}`\n\n")
            out.write("| field | type |\n|---|---|\n")
            for f in s.fields:
                out.write(f"| `{f.name}` | `{f.type}` |\n")
            out.write("\n")

    out.write("## Functions\n\n")
    for group_name in sorted(grouped):
        out.write(f"### {group_name}\n\n")
        for fn in grouped[group_name]:
            out.write(f"- `{_format_function_signature(fn)}`\n")
        out.write("\n")

    return out.getvalue()
