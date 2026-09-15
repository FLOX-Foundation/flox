"""Compare function signatures, macro constants, and struct fields across
two C headers.

Used by CI to confirm that codegen output is signature-equivalent to the
hand-written flox_capi.h (or, after T014, that the live flox_capi.h matches
what codegen would produce).

Three kinds of declaration are compared, each structurally:
- functions: name, return type, parameter count, parameter types (names
  ignored — different headers use different param names freely)
- object-like macros (`#define NAME value`): name, replacement-token text
- structs (`typedef struct { ... } Name;`): field count, field names and
  types, in declaration order

Enums, unions, and function-pointer typedefs are not covered — see
`_index_macros` / `_index_structs` docstrings for why the two kinds above
needed a second parse pass. A field or macro renamed, reordered, added, or
removed between `expected` and `actual` is exactly as blocking as a
function signature mismatch; this was not always true (see git history: a
prior version only ever looked at K.FUNCTION_DECL, so deleting every
FLOX_SIGNAL_TYPE_* constant or slipping an extra field into FloxSignal
passed `--require-full-coverage` clean).

Type comparison preserves typedef names (`int64_t`, `size_t`,
`FloxStrategyHandle`) rather than canonicalizing through them. That keeps
the comparison platform-invariant: macOS resolves `int64_t` to `long long`
and Linux x86_64 resolves it to `long`, but both just say `int64_t` when
we read the spelling without forcing canonical resolution.

Declarations present in `expected` but missing from `actual` are reported
as errors when `require_full_coverage` is set (T014 mode), informational
otherwise. Declarations in `actual` but not `expected` are always
informational — the codegen output may legitimately cover a subset of the
live header during the prototype phase. Field/value mismatches on a
declaration present in both are always blocking, coverage flag or not.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

from . import extractor


def _normalize_type(spelling: str) -> str:
    """Collapse runs of whitespace; trim leading/trailing space.

    Different libclang versions / hosts spell the same C type with subtly
    different whitespace (`const double*` vs `const double *` vs
    `const  double  *`); normalizing turns them all into a single
    canonical form.
    """
    return re.sub(r"\s+", " ", spelling).strip()


@dataclass(frozen=True)
class FuncSig:
    name: str
    return_type: str
    param_types: Tuple[str, ...]


def _index_header(path: Path, *, include_dirs: Iterable[Path] = ()) -> Dict[str, FuncSig]:
    """Parse a C header (or the spec) and return {name: FuncSig}.

    For headers that aren't FLOX_EXPORT-annotated (e.g. the existing
    flox_capi.h), every free function is collected.
    """
    extractor._ensure_libclang_loaded()
    import clang.cindex

    args: List[str] = ["-x", "c", "-std=c11"]
    for d in include_dirs:
        args += ["-I", str(d)]
    for d in extractor._discover_system_includes():
        args += ["-I", d]

    index = clang.cindex.Index.create()
    tu = index.parse(str(path), args=args, options=0)

    diags = [d for d in tu.diagnostics if d.severity >= clang.cindex.Diagnostic.Error]
    if diags:
        msg = "\n".join(f"  {d.location}: {d.spelling}" for d in diags)
        raise RuntimeError(f"libclang errors parsing {path}:\n{msg}")

    out: Dict[str, FuncSig] = {}

    def visit(c):
        K = clang.cindex.CursorKind
        if c.kind in (K.NAMESPACE, K.UNEXPOSED_DECL, K.LINKAGE_SPEC):
            for ch in c.get_children():
                visit(ch)
            return
        if c.kind == K.FUNCTION_DECL:
            params = tuple(
                _normalize_type(arg.type.spelling) for arg in c.get_arguments()
            )
            sig = FuncSig(
                name=c.spelling,
                return_type=_normalize_type(c.result_type.spelling),
                param_types=params,
            )
            out[c.spelling] = sig

    for ch in tu.cursor.get_children():
        visit(ch)
    return out


@dataclass(frozen=True)
class MacroSig:
    name: str
    value: str  # normalized replacement-token text; "" for a bare `#define NAME`


def _index_macros(path: Path, *, include_dirs: Iterable[Path] = ()) -> Dict[str, MacroSig]:
    """Parse a C header and return {name: MacroSig} for object-like macros
    `#define`d directly in `path` (not pulled in transitively via #include).

    Delegates the actual libclang walk to `extractor.macro_definition_cursors`
    -- the same cursor walk the spec's macro-constant IDL group is built
    from (see `extractor.extract_macros`), so the golden/live comparison and
    the spec extraction can never see a different macro set for the same
    file just because one of them drifted its own copy of this walk.
    PARSE_DETAILED_PROCESSING_RECORD is required for libclang to emit
    MACRO_DEFINITION cursors at all -- `_index_header` above parses with
    `options=0`, so a whole class of the C ABI (every `FLOX_SIGNAL_TYPE_*`
    constant, at the time this module was written) was invisible to it
    regardless of what the diff logic did with it.
    """
    args: List[str] = ["-x", "c", "-std=c11"]
    for d in include_dirs:
        args += ["-I", str(d)]
    for d in extractor._discover_system_includes():
        args += ["-I", d]

    out: Dict[str, MacroSig] = {}
    for name, value, _line in extractor.macro_definition_cursors(path, args=args):
        out[name] = MacroSig(name=name, value=value)
    return out


@dataclass(frozen=True)
class FieldSig:
    name: str
    type: str


@dataclass(frozen=True)
class StructSig:
    name: str
    fields: Tuple[FieldSig, ...]


def _index_structs(path: Path, *, include_dirs: Iterable[Path] = ()) -> Dict[str, StructSig]:
    """Parse a C header and return {typedef_name: StructSig} for every
    `typedef struct { ... } Name;` declared directly in `path`.

    Anonymous-struct-plus-typedef is the only shape flox_capi.h uses for
    its data structs (FloxSignal, FloxOrder, ...), so keying off the
    TYPEDEF_DECL's underlying struct declaration covers all of them. A
    struct with no fields (an opaque marker type, if one ever appears) is
    skipped rather than compared as an empty field list.
    """
    extractor._ensure_libclang_loaded()
    import clang.cindex

    args: List[str] = ["-x", "c", "-std=c11"]
    for d in include_dirs:
        args += ["-I", str(d)]
    for d in extractor._discover_system_includes():
        args += ["-I", d]

    index = clang.cindex.Index.create()
    tu = index.parse(str(path), args=args, options=0)

    diags = [d for d in tu.diagnostics if d.severity >= clang.cindex.Diagnostic.Error]
    if diags:
        msg = "\n".join(f"  {d.location}: {d.spelling}" for d in diags)
        raise RuntimeError(f"libclang errors parsing {path}:\n{msg}")

    resolved = path.resolve()
    out: Dict[str, StructSig] = {}

    def visit(c):
        K = clang.cindex.CursorKind
        if c.kind in (K.NAMESPACE, K.UNEXPOSED_DECL, K.LINKAGE_SPEC):
            for ch in c.get_children():
                visit(ch)
            return
        if c.kind == K.TYPEDEF_DECL:
            loc_file = c.location.file
            if loc_file is not None and Path(str(loc_file)).resolve() == resolved:
                decl = c.underlying_typedef_type.get_declaration()
                if decl.kind in (K.STRUCT_DECL, K.UNION_DECL):
                    fields = tuple(
                        FieldSig(name=f.spelling, type=_normalize_type(f.type.spelling))
                        for f in decl.get_children()
                        if f.kind == K.FIELD_DECL
                    )
                    if fields:
                        out[c.spelling] = StructSig(name=c.spelling, fields=fields)

    for ch in tu.cursor.get_children():
        visit(ch)
    return out


@dataclass
class Mismatch:
    name: str
    reason: str  # "missing" | "return-type" | "arity" | "param-type"
    detail: str


def diff(
    expected: Dict[str, FuncSig], actual: Dict[str, FuncSig]
) -> Tuple[List[Mismatch], List[str]]:
    """Compare expected (the live flox_capi.h, say) vs actual (codegen output).

    Returns (mismatches, extra_in_actual). Mismatches are blocking errors.
    extra_in_actual is non-blocking — a slice prototype legitimately exports
    fewer functions than the live header.
    """
    mismatches: List[Mismatch] = []
    extra: List[str] = sorted(set(actual) - set(expected))

    for name, want in expected.items():
        got = actual.get(name)
        if got is None:
            # Not produced by codegen — that's a coverage gap, not necessarily an
            # error during prototype. The CLI decides whether to fail on it.
            continue
        if want.return_type != got.return_type:
            mismatches.append(
                Mismatch(
                    name=name,
                    reason="return-type",
                    detail=f"expected {want.return_type!r}, got {got.return_type!r}",
                )
            )
            continue
        if len(want.param_types) != len(got.param_types):
            mismatches.append(
                Mismatch(
                    name=name,
                    reason="arity",
                    detail=(
                        f"expected {len(want.param_types)} params, "
                        f"got {len(got.param_types)}"
                    ),
                )
            )
            continue
        for i, (w, g) in enumerate(zip(want.param_types, got.param_types)):
            if w != g:
                mismatches.append(
                    Mismatch(
                        name=name,
                        reason="param-type",
                        detail=f"param[{i}]: expected {w!r}, got {g!r}",
                    )
                )
                break

    return mismatches, extra


def diff_macros(
    expected: Dict[str, MacroSig], actual: Dict[str, MacroSig]
) -> Tuple[List[Mismatch], List[str]]:
    """Same contract as diff(), for object-like macro constants."""
    mismatches: List[Mismatch] = []
    extra: List[str] = sorted(set(actual) - set(expected))

    for name, want in expected.items():
        got = actual.get(name)
        if got is None:
            continue
        if want.value != got.value:
            mismatches.append(
                Mismatch(
                    name=name,
                    reason="macro-value",
                    detail=f"expected {want.value!r}, got {got.value!r}",
                )
            )

    return mismatches, extra


def diff_structs(
    expected: Dict[str, StructSig], actual: Dict[str, StructSig]
) -> Tuple[List[Mismatch], List[str]]:
    """Same contract as diff(), for `typedef struct {...} Name;` field lists."""
    mismatches: List[Mismatch] = []
    extra: List[str] = sorted(set(actual) - set(expected))

    for name, want in expected.items():
        got = actual.get(name)
        if got is None:
            continue
        if len(want.fields) != len(got.fields):
            mismatches.append(
                Mismatch(
                    name=name,
                    reason="struct-field-count",
                    detail=(
                        f"expected {len(want.fields)} fields, "
                        f"got {len(got.fields)}"
                    ),
                )
            )
            continue
        for i, (w, g) in enumerate(zip(want.fields, got.fields)):
            if w.name != g.name or w.type != g.type:
                mismatches.append(
                    Mismatch(
                        name=name,
                        reason="struct-field",
                        detail=(
                            f"field[{i}]: expected {w.name} {w.type!r}, "
                            f"got {g.name} {g.type!r}"
                        ),
                    )
                )
                break

    return mismatches, extra


# Pre-existing coverage gaps, exempted from the "missing" list until a
# dedicated follow-up closes them. Do NOT add a name here to silence a NEW
# gap your own change introduced -- exempt only what already exists after
# teaching the spec about it, and delete the entry once something actually
# closes it.
#
# This used to hold fifteen names (the FLOX_SIGNAL_TYPE_* order-type/signal
# codes and FLOX_CAPI_ABI_VERSION) because the codegen spec had no
# macro-constant IDL group at all -- every `#define` in flox_capi.h was
# invisible to golden/flox_capi.{h,codon,md} regardless of what the diff
# logic did with it, and the list grew by three and then one more name
# across two separate audit batches with no fix in sight. The spec and the
# emitters now know about macro constants (`flox::export_macro(group=...)`
# markers in flox_capi_spec.hpp, emitted by emit_capi/emit_codon/emit_llms),
# all fifteen are exported through it, and golden/flox_capi.h now carries
# every one of them -- so the set is empty. A name added here again means a
# macro exists in the live header with no `flox::export_macro(...)` marker
# in the spec; add the marker instead of exempting it.
KNOWN_MISSING_MACROS: set = set()


def check(
    *,
    expected_header: Path,
    actual_header: Path,
    include_dirs: Iterable[Path] = (),
    require_full_coverage: bool = False,
) -> Tuple[List[Mismatch], List[str], List[str]]:
    """High-level: parse both headers and diff functions, macros, and structs.

    Returns (mismatches, missing_from_actual, extra_in_actual), pooled across
    all three declaration kinds. `mismatches` is always blocking.
    `missing_from_actual` is blocking only when `require_full_coverage` is
    True (i.e. T014 mode).
    """
    expected_funcs = _index_header(expected_header, include_dirs=include_dirs)
    actual_funcs = _index_header(actual_header, include_dirs=include_dirs)
    expected_macros = _index_macros(expected_header, include_dirs=include_dirs)
    actual_macros = _index_macros(actual_header, include_dirs=include_dirs)
    expected_structs = _index_structs(expected_header, include_dirs=include_dirs)
    actual_structs = _index_structs(actual_header, include_dirs=include_dirs)

    mismatches, extra = diff(expected_funcs, actual_funcs)
    missing = sorted(set(expected_funcs) - set(actual_funcs))

    macro_mismatches, macro_extra = diff_macros(expected_macros, actual_macros)
    mismatches += macro_mismatches
    extra += macro_extra
    missing += sorted(
        (set(expected_macros) - set(actual_macros)) - KNOWN_MISSING_MACROS
    )

    struct_mismatches, struct_extra = diff_structs(expected_structs, actual_structs)
    mismatches += struct_mismatches
    extra += struct_extra
    missing += sorted(set(expected_structs) - set(actual_structs))

    return mismatches, missing, extra
