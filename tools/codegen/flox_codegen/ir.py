"""Intermediate representation for FLOX C API surface.

The IR is the contract between the libclang-based extractor and the per-binding
emitters. Each emitter (emit_capi, emit_pyi, emit_dts, emit_codon, emit_llms)
operates only on this IR — never directly on libclang cursors — so adding a new
emitter is one new file with no schema change.

The vocabulary mirrors what FLOX_EXPORT annotations carry; see
include/flox/capi/flox_export.h for the source-side reference.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


@dataclass(frozen=True)
class Param:
    """A single function parameter."""

    name: str
    type: str  # canonical C type spelling, e.g. "const double*", "int64_t"


@dataclass(frozen=True)
class Function:
    """A free C function exported via FLOX_EXPORT."""

    name: str  # emitted C symbol, e.g. "flox_indicator_ema"
    return_type: str  # canonical C return type
    params: Tuple[Param, ...]
    annotations: Dict[str, str] = field(default_factory=dict)
    source_location: Optional[str] = None  # "file:line" — for error reporting


@dataclass(frozen=True)
class StructField:
    name: str
    type: str
    array_size: Optional[int] = None  # for "uint8_t _pad[2]"-style fixed arrays


@dataclass(frozen=True)
class Struct:
    name: str
    fields: Tuple[StructField, ...]


@dataclass(frozen=True)
class HandleTypedef:
    """typedef void* FloxXHandle — opaque-handle declaration.

    `alias_of` is non-None when this declaration is a typedef alias of an
    earlier handle (e.g. `typedef FloxIndicatorGraphHandle FloxStreamingGraphHandle`).
    Aliases emit as `typedef <alias_of> <name>;` rather than `typedef void* <name>`.
    """

    name: str
    alias_of: Optional[str] = None  # e.g. "FloxStrategyHandle"


@dataclass(frozen=True)
class FunctionPointerTypedef:
    """typedef void (*FloxOnX)(args) — callback function-pointer typedef."""

    name: str
    return_type: str
    params: Tuple[Param, ...]


@dataclass(frozen=True)
class EnumValue:
    name: str
    value: Optional[int] = None  # None means "let C compiler auto-assign"


@dataclass(frozen=True)
class Enum:
    """typedef enum { ... } FloxFooKind — C-style enum bound to a typedef."""

    name: str
    values: Tuple[EnumValue, ...]


@dataclass
class Module:
    """The full IR — every emitter consumes one Module instance."""

    handles: List[HandleTypedef] = field(default_factory=list)
    structs: List[Struct] = field(default_factory=list)
    function_pointers: List[FunctionPointerTypedef] = field(default_factory=list)
    enums: List[Enum] = field(default_factory=list)
    functions: List[Function] = field(default_factory=list)

    def functions_by_group(self) -> Dict[str, List[Function]]:
        """Group exported functions by their `group=` annotation.

        Functions without a group fall under "_ungrouped". Order within each
        group is preserved from the spec source order.
        """
        out: Dict[str, List[Function]] = {}
        for fn in self.functions:
            g = fn.annotations.get("group", "_ungrouped")
            out.setdefault(g, []).append(fn)
        return out
