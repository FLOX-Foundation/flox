"""IR → Codon `from C import ...` stub emitter.

Produces a Codon source file with one `from C import name(args) -> ret` line
per exported C function. Handles, structs, and function-pointers all collapse
to `cobj` on the Codon side because Codon FFI doesn't model opaque C types
beyond the byte level — callers are expected to know the lifecycle.

The generated file is a flat reference: bindings under `codon/flox/*.codon`
import only the functions they actually use, and may layer richer Codon
classes on top. This emitter does NOT produce those wrapper classes; it
only declares the FFI surface so they don't drift from `flox_capi.h`.
"""
from __future__ import annotations

import re
from io import StringIO
from typing import List

from . import ir


# ── Type mapping ─────────────────────────────────────────────────────

# Canonical-form C type → Codon type. Order matters: more specific patterns
# come first.
_PRIMITIVE_MAP = {
    "void": None,  # only valid as return type
    "uint8_t": "u8",
    "uint16_t": "u16",
    "uint32_t": "u32",
    "uint64_t": "u64",
    "int8_t": "i8",
    "int16_t": "i16",
    "int32_t": "i32",
    "int64_t": "i64",
    "size_t": "int",
    "ssize_t": "int",
    "double": "float",
    "float": "float32",
    "char": "byte",
    "int": "int",
    "long": "int",
    "unsigned int": "u32",
    "unsigned long": "u64",
    "_Bool": "bool",
    "bool": "bool",
}

# Match a single "*"-ended pointer; the body is the pointee. Keep `const`
# since pointers to const T have the same Codon representation as T*.
_POINTER_RE = re.compile(r"^\s*(?:const\s+)?(?P<inner>.*?)\s*\*\s*$")


def _strip_consts(s: str) -> str:
    """Drop `const` qualifiers anywhere in the type spelling.

    Codon's FFI doesn't represent const at all; `const T*` and `T*` are the
    same Codon shape. This makes type matching independent of where the
    const sits.
    """
    return re.sub(r"\bconst\b", "", s).strip()


def map_type(c_type: str, *, is_return: bool = False) -> str:
    """Map a canonical C type spelling to the Codon FFI type spelling.

    Pointer-to-aggregate / pointer-to-handle → `cobj`. Pointer-to-scalar →
    `Ptr[T]` where T is the scalar's Codon name. void → empty string in
    return position, treated as no return type.
    """
    # Drop every `const` keyword regardless of position. The remaining
    # spelling is a const-free type with optional whitespace; we then
    # collapse runs of whitespace.
    s = re.sub(r"\s+", " ", _strip_consts(c_type)).strip()

    if s in _PRIMITIVE_MAP:
        prim = _PRIMITIVE_MAP[s]
        if prim is None:
            return ""  # void
        return prim

    # Strip a single trailing `*` and recurse for the pointee.
    m = _POINTER_RE.match(s)
    if m:
        inner = m.group("inner").strip()

        # `void *` → `cobj`.
        if inner == "void":
            return "cobj"

        # `char *` (typically `const char*` after const-stripping) → `cobj` —
        # matches the existing codon binding convention. Callers pass
        # `.c_str()` from a Codon str.
        if inner == "char":
            return "cobj"

        # Pointer-to-pointer: `const char* const*` collapses to `Ptr[cobj]`.
        if inner.endswith("*"):
            inner_codon = map_type(inner)
            return f"Ptr[{inner_codon}]"

        if inner in _PRIMITIVE_MAP:
            prim = _PRIMITIVE_MAP[inner]
            return f"Ptr[{prim}]" if prim is not None else "cobj"

        # Pointer-to-aggregate (struct, opaque handle, callback typedef, etc.).
        # Codon erases these to `cobj` — the wrapper layer reconstructs typing.
        return "cobj"

    # Bare aggregate (Flox*Handle, FloxXCallbacks, FloxBar): cobj.
    if s.startswith("Flox"):
        return "cobj"

    # Unknown — leave as-is so the developer can patch the emitter.
    return s


def _emit_function(out: StringIO, fn: ir.Function) -> None:
    arg_types = [map_type(p.type) for p in fn.params]
    ret = map_type(fn.return_type, is_return=True)

    arg_list = ", ".join(arg_types)
    if ret:
        out.write(f"from C import {fn.name}({arg_list}) -> {ret}\n")
    else:
        out.write(f"from C import {fn.name}({arg_list})\n")


def emit(module: ir.Module) -> str:
    """Render the IR as a Codon FFI declaration file."""
    out = StringIO()
    out.write("# GENERATED — do not edit by hand.\n")
    out.write("# Source: include/flox/capi/flox_capi_spec.hpp\n")
    out.write("# Tool:   tools/codegen/flox_codegen/emit_codon.py\n")
    out.write("#\n")
    out.write("# Codon `from C import ...` declarations for the FLOX C API.\n")
    out.write("# Re-exported through `codon/flox/<module>.codon` files; this\n")
    out.write("# file is the flat reference, not directly imported by user code.\n")
    out.write("\n")

    grouped = module.functions_by_group()
    for group_name in sorted(grouped):
        out.write(f"# ── {group_name} ──\n")
        for fn in grouped[group_name]:
            _emit_function(out, fn)
        out.write("\n")

    return out.getvalue()
