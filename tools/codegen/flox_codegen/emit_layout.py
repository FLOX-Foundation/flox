"""IR → struct-layout tables for the bindings that read events as raw bytes.

Codon has no struct declarations to work from: `emit_codon` maps every
aggregate to `cobj`, so `codon/flox/dispatch.codon` reaches into
`FloxTradeData`, `FloxBarData`, `FloxOrderEventData` and `FloxSymbolContext`
by byte offset, and `codon/flox/strategy.codon` does the same for `FloxBar`.
Those offsets used to be typed in by hand, with nothing connecting them to
the header: widening a field or reordering two moved every offset after it
and no part of the build noticed.

This emitter computes the offsets from the same IDL the C header is
generated from, and writes them twice:

* `include/flox/capi/flox_capi_layout.h` — a table the C++ test
  `tests/test_capi_event_layout.cpp` compares against `offsetof`/`sizeof`,
  so a computed offset that disagrees with the compiler fails the build.
* `codon/flox/layout.codon` — the constants the Codon binding imports.

The two are emitted from one layout pass, so they cannot disagree with each
other, and the C++ test is what keeps the pass itself honest.

Layout rules are the platform C ABI ones: a field sits at the next offset
aligned to its own alignment, and the struct's size is rounded up to its
largest member alignment. That is the System V / AArch64 / MSVC x64 layout
for the scalar types used here; the C++ test is the check that the target
actually agrees.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from io import StringIO
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from . import ir
from .emit_capi import _run_clang_format


# The structs a binding reads through raw memory. Every other struct on the
# boundary is passed to C by pointer and never decomposed by hand, so a table
# for it would describe a layout nothing depends on. Keep this list and
# `expectedStructs()` in tests/test_capi_event_layout.cpp in step: the test
# fails on an entry it cannot account for.
LAYOUT_STRUCTS: Tuple[str, ...] = (
    "FloxTradeData",
    "FloxBookSnapshot",
    "FloxSymbolContext",
    "FloxBarData",
    "FloxOrderEventData",
    "FloxBar",
)

# (size, alignment) per scalar spelling, LP64/LLP64 with 8-byte pointers --
# every target FLOX ships binaries for. `long` is deliberately absent: it is
# 4 bytes on Windows and 8 elsewhere, so a struct on this boundary may not
# use it, and a spelling with no entry here is an error rather than a guess.
_SCALARS: Dict[str, Tuple[int, int]] = {
    "char": (1, 1),
    "signed char": (1, 1),
    "unsigned char": (1, 1),
    "int8_t": (1, 1),
    "uint8_t": (1, 1),
    "_Bool": (1, 1),
    "bool": (1, 1),
    "int16_t": (2, 2),
    "uint16_t": (2, 2),
    "short": (2, 2),
    "int32_t": (4, 4),
    "uint32_t": (4, 4),
    "int": (4, 4),
    "unsigned int": (4, 4),
    "float": (4, 4),
    "int64_t": (8, 8),
    "uint64_t": (8, 8),
    "double": (8, 8),
    "size_t": (8, 8),
}

_POINTER = (8, 8)

_ARRAY_RE = re.compile(r"^(?P<base>.*?)\s*\[(?P<count>\d+)\]$")


@dataclass(frozen=True)
class FieldLayout:
    struct: str
    name: str
    offset: int
    size: int


@dataclass(frozen=True)
class StructLayout:
    name: str
    size: int
    alignment: int
    fields: Tuple[FieldLayout, ...]


class LayoutError(RuntimeError):
    """A type on the event boundary the layout pass cannot size."""


def _normalize(type_spelling: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"\bconst\b", "", type_spelling)).strip()


def _size_align(type_spelling: str, structs: Dict[str, ir.Struct],
                cache: Dict[str, StructLayout]) -> Tuple[int, int]:
    """Size and alignment of one field type."""
    spelling = _normalize(type_spelling)

    m = _ARRAY_RE.match(spelling)
    if m:
        size, align = _size_align(m.group("base"), structs, cache)
        return size * int(m.group("count")), align

    if spelling.endswith("*"):
        return _POINTER

    if spelling in _SCALARS:
        return _SCALARS[spelling]

    if spelling in structs:
        nested = _layout_struct(spelling, structs, cache)
        return nested.size, nested.alignment

    raise LayoutError(
        f"no size known for `{type_spelling}`; add it to _SCALARS or keep it "
        "off the event boundary")


def _layout_struct(name: str, structs: Dict[str, ir.Struct],
                   cache: Dict[str, StructLayout]) -> StructLayout:
    if name in cache:
        return cache[name]
    if name not in structs:
        raise LayoutError(f"struct `{name}` is not declared in the IDL spec")

    offset = 0
    alignment = 1
    fields: List[FieldLayout] = []
    for field in structs[name].fields:
        size, align = _size_align(field.type, structs, cache)
        offset = (offset + align - 1) // align * align
        # Padding is laid out but not exported: it moves the fields after it
        # and nothing is allowed to read it.
        if not field.name.startswith("_"):
            fields.append(FieldLayout(name, field.name, offset, size))
        offset += size
        alignment = max(alignment, align)

    size = (offset + alignment - 1) // alignment * alignment
    layout = StructLayout(name, size, alignment, tuple(fields))
    cache[name] = layout
    return layout


def compute(module: ir.Module,
            names: Tuple[str, ...] = LAYOUT_STRUCTS) -> List[StructLayout]:
    """Lay out `names` from the IDL, in the order given."""
    structs = {s.name: s for s in module.structs}
    cache: Dict[str, StructLayout] = {}
    return [_layout_struct(n, structs, cache) for n in names]


# ── C header ──────────────────────────────────────────────────────────

_HEADER_PROLOGUE = """\
/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * GENERATED — do not edit by hand.
 * Source: include/flox/capi/flox_capi_spec.hpp
 * Tool:   tools/codegen/flox_codegen/emit_layout.py
 *
 * Byte layout of the event structs a binding may read through raw memory.
 * codon/flox/layout.codon is generated from the same pass; the offsets there
 * are these offsets. tests/test_capi_event_layout.cpp compares every entry
 * below against offsetof/sizeof on the real struct, so a layout the compiler
 * disagrees with fails the build instead of a user's strategy.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct
  {
    const char* struct_name;
    const char* field_name;
    size_t offset;
    size_t size;
  } FloxFieldLayout;

  typedef struct
  {
    const char* struct_name;
    size_t size;
    size_t alignment;
  } FloxStructLayout;

"""

_HEADER_EPILOGUE = """
#ifdef __cplusplus
}
#endif
"""


def emit_header(module: ir.Module, *, format: bool = True,
                style_file: Optional[Path] = None,
                names: Tuple[str, ...] = LAYOUT_STRUCTS) -> str:
    layouts = compute(module, names)
    out = StringIO()
    out.write(_HEADER_PROLOGUE)

    out.write("  static const FloxFieldLayout kFloxEventLayout[] = {\n")
    for layout in layouts:
        for f in layout.fields:
            out.write(f'      {{"{f.struct}", "{f.name}", {f.offset}, {f.size}}},\n')
    out.write("  };\n\n")

    out.write("  static const FloxStructLayout kFloxEventStructLayout[] = {\n")
    for layout in layouts:
        out.write(f'      {{"{layout.name}", {layout.size}, {layout.alignment}}},\n')
    out.write("  };\n")

    out.write(_HEADER_EPILOGUE)
    text = out.getvalue()
    if format:
        text = _run_clang_format(text, style_file=style_file)
    return text


# ── Codon module ──────────────────────────────────────────────────────

# Constant prefix per struct. The names the Codon binding already used are
# the contract here -- dispatch.codon and strategy.codon import them by name.
_CODON_PREFIX: Dict[str, str] = {
    "FloxTradeData": "_TRADE",
    "FloxBookSnapshot": "_BOOK",
    "FloxSymbolContext": "_CTX",
    "FloxBarData": "_BAR",
    "FloxOrderEventData": "_EV",
    "FloxBar": "_FLOXBAR",
}

# Fields whose constant is not the field name upper-cased, again because the
# binding already spells them this way.
_CODON_FIELD_ALIASES: Dict[str, str] = {
    "bar_type": "TYPE",
    "bar_type_param": "TYPE_PARAM",
    "quantity_raw": "QTY_RAW",
    "exchange_ts_ns": "TS_NS",
    "start_time_ns": "START_NS",
    "end_time_ns": "END_NS",
    "distance_to_best_ticks": "DISTANCE_TICKS",
}

_CODON_PROLOGUE = """\
# GENERATED — do not edit by hand.
# Source: include/flox/capi/flox_capi_spec.hpp
# Tool:   tools/codegen/flox_codegen/emit_layout.py
#
# Byte offsets of the C event structs, for the Codon binding's readers.
# Codon's FFI models every aggregate as an opaque cobj, so an event payload
# is read field by field out of the pointer the engine hands over. These are
# the offsets to read it at.
#
# The same pass writes include/flox/capi/flox_capi_layout.h, and
# tests/test_capi_event_layout.cpp compares that table against offsetof on
# the real structs -- so an offset here that the C compiler disagrees with
# fails CI rather than delivering a wrong number to a strategy.

"""


def _codon_name(prefix: str, field: str) -> str:
    return f"{prefix}_{_CODON_FIELD_ALIASES.get(field, field.upper())}"


def emit_codon(module: ir.Module,
               names: Tuple[str, ...] = LAYOUT_STRUCTS) -> str:
    layouts = compute(module, names)
    out = StringIO()
    out.write(_CODON_PROLOGUE)
    for layout in layouts:
        prefix = _CODON_PREFIX.get(layout.name)
        if prefix is None:
            raise LayoutError(
                f"no Codon constant prefix for `{layout.name}`; add one to "
                "_CODON_PREFIX")
        out.write(f"# {layout.name}\n")
        for f in layout.fields:
            out.write(f"{_codon_name(prefix, f.name)} = {f.offset}\n")
        out.write(f"{prefix}_SIZE = {layout.size}\n")
        out.write(f"{prefix}_ALIGN = {layout.alignment}\n")
        out.write("\n")
    return out.getvalue()
