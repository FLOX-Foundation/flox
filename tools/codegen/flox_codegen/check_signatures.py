"""Compare function signatures across two C headers.

Used by CI to confirm that codegen output is signature-equivalent to the
hand-written flox_capi.h (or, after T014, that the live flox_capi.h matches
what codegen would produce).

Equivalence is structural:
- function name
- canonical return type
- parameter count
- parameter canonical types (names ignored — different headers use different
  param names freely)

Functions present in `expected` but missing from `actual` are reported as
errors. Functions in `actual` but not `expected` are reported as warnings
(the codegen output may legitimately cover a subset of the live header
during the prototype phase).
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

from . import extractor


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
            params = tuple(arg.type.get_canonical().spelling for arg in c.get_arguments())
            sig = FuncSig(
                name=c.spelling,
                return_type=c.result_type.get_canonical().spelling,
                param_types=params,
            )
            out[c.spelling] = sig

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


def check(
    *,
    expected_header: Path,
    actual_header: Path,
    include_dirs: Iterable[Path] = (),
    require_full_coverage: bool = False,
) -> Tuple[List[Mismatch], List[str], List[str]]:
    """High-level: parse both headers and diff.

    Returns (mismatches, missing_from_actual, extra_in_actual).
    `mismatches` is always blocking. `missing_from_actual` is blocking only when
    `require_full_coverage` is True (i.e. T014 mode).
    """
    expected = _index_header(expected_header, include_dirs=include_dirs)
    actual = _index_header(actual_header, include_dirs=include_dirs)

    mismatches, extra = diff(expected, actual)
    missing = sorted(set(expected) - set(actual))
    return mismatches, missing, extra
