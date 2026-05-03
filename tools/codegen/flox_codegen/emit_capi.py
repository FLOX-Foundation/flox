"""IR → flox_capi.h emitter.

Produces a C header with the same conventions as the existing hand-written
include/flox/capi/flox_capi.h:

- Wrapped in `extern "C" { ... }` under `#ifdef __cplusplus`.
- Two-space indent inside the extern block.
- Section banners with the `group=` annotation as the title.
- Order: handles → event-data structs → fnptr typedefs → callback bundles
  → grouped functions.

Output is run through `clang-format` (with the repo's `.clang-format` config)
when the binary is available, so committed golden output is byte-stable
under FLOX's format gate. When clang-format is missing, raw emitter output
is returned with a single trailing newline — the unit tests cover both
paths.
"""
from __future__ import annotations

import shutil
import subprocess
from io import StringIO
from pathlib import Path
from typing import List, Optional

from . import ir


HEADER_PROLOGUE = """\
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
 * Tool:   tools/codegen/flox_codegen/emit_capi.py
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern \"C\"
{
#endif
"""

HEADER_EPILOGUE = """
#ifdef __cplusplus
}
#endif
"""

# Order in which groups appear in the emitted header. Groups not listed appear
# at the end in alphabetical order. Mirrors the section flow of the existing
# hand-written flox_capi.h.
GROUP_ORDER = (
    "registry",
    "strategy",
    "signal",
    "context",
    "fixed_point",
    "indicator",
    "target",
    "stat",
)

GROUP_TITLES = {
    "registry": "Symbol registry",
    "strategy": "Strategy lifecycle",
    "signal": "Signal emission (returns OrderId, 0 on failure)",
    "context": "Context queries",
    "fixed_point": "Fixed-point conversion helpers",
    "indicator": "Indicator functions (stateless, array-in/array-out)",
    "target": "Targets (forward-looking labels, batch only)",
    "stat": "Statistics",
}


def _banner(title: str, *, indent: str = "  ") -> str:
    bar = "=" * 60
    return f"{indent}// {bar}\n{indent}// {title}\n{indent}// {bar}\n"


def _emit_handles(out: StringIO, handles: List[ir.HandleTypedef]) -> None:
    if not handles:
        return
    out.write("\n")
    out.write(_banner("Opaque handles"))
    out.write("\n")
    for h in handles:
        out.write(f"  typedef void* {h.name};\n")


_ARRAY_SUFFIX_RE = __import__("re").compile(r"^(.*?)(\[[\w\s\*]+\])\s*$")


def _split_array_suffix(type_spelling: str) -> tuple:
    """libclang renders fixed arrays as `uint8_t[2]`; C declarators put the
    `[2]` after the field name. This splits the type into (base, suffix).

    Returns (base_type, "" | "[N]").
    """
    m = _ARRAY_SUFFIX_RE.match(type_spelling.strip())
    if not m:
        return type_spelling, ""
    return m.group(1).strip(), m.group(2)


def _emit_structs(
    out: StringIO, structs: List[ir.Struct], *, banner: str = "Flat event structs (C-compatible)"
) -> None:
    if not structs:
        return
    out.write("\n")
    out.write(_banner(banner))
    out.write("\n")
    for s in structs:
        out.write("  typedef struct\n")
        out.write("  {\n")
        for f in s.fields:
            base, suffix = _split_array_suffix(f.type)
            out.write(f"    {base} {f.name}{suffix};\n")
        out.write(f"  }} {s.name};\n\n")


def _emit_function_pointers(
    out: StringIO, fps: List[ir.FunctionPointerTypedef]
) -> None:
    if not fps:
        return
    out.write(_banner("Callback function pointer types"))
    out.write("\n")
    for fp in fps:
        params = ", ".join(p.type for p in fp.params) or "void"
        out.write(f"  typedef {fp.return_type} (*{fp.name})({params});\n")
    out.write("\n")


def _emit_function(out: StringIO, fn: ir.Function, *, indent: str = "  ") -> None:
    """Emit a single function declaration with simple wrapping."""
    sig_head = f"{indent}{fn.return_type} {fn.name}("
    if fn.params:
        param_strs = [
            (f"{p.type} {p.name}".rstrip() if p.name else p.type)
            for p in fn.params
        ]
    else:
        param_strs = ["void"]

    one_line = sig_head + ", ".join(param_strs) + ");"
    if len(one_line) <= 100:
        out.write(one_line + "\n")
        return

    # Wrap: first param after `(`, subsequent params aligned beneath it.
    cont_pad = " " * len(sig_head)
    out.write(sig_head + param_strs[0])
    for p in param_strs[1:]:
        candidate_extra = f", {p}"
        # If the *previous* line still fits, add inline.
        cur_line = out.getvalue().rsplit("\n", 1)[-1]
        if len(cur_line) + len(candidate_extra) <= 100:
            out.write(candidate_extra)
        else:
            out.write(",\n" + cont_pad + p)
    out.write(");\n")


def _ordered_groups(groups: dict) -> list:
    listed = [g for g in GROUP_ORDER if g in groups]
    extras = sorted(set(groups) - set(GROUP_ORDER))
    return listed + extras


def _partition_structs(
    structs: List[ir.Struct], fp_names: set
) -> tuple:
    """Split structs into (event_data, callback_bundles).

    A struct is a callback-bundle if any of its field types names a function-
    pointer typedef. That ordering matters at emit time: event data structs
    are referenced by fnptr typedefs (e.g. FloxOnTradeCallback uses
    FloxSymbolContext), and callback bundles reference fnptr typedefs (e.g.
    FloxStrategyCallbacks holds FloxOnTradeCallback). So the dependency chain
    is event → fnptr → bundle.
    """
    event_data: List[ir.Struct] = []
    bundles: List[ir.Struct] = []
    for s in structs:
        is_bundle = any(
            _split_array_suffix(f.type)[0] in fp_names for f in s.fields
        )
        if is_bundle:
            bundles.append(s)
        else:
            event_data.append(s)
    return event_data, bundles


def _run_clang_format(text: str, *, style_file: Optional[Path] = None) -> str:
    """Pipe `text` through clang-format. Returns input unchanged on missing tool.

    `style_file` should point at an existing `.clang-format` config; passes the
    enclosing directory to `clang-format -style=file -assume-filename=...`.
    Errors from clang-format propagate (broken config is a real bug — better to
    fail loudly than ship malformed output).
    """
    binary = shutil.which("clang-format")
    if binary is None:
        return text

    args = [binary, "-style=file"]
    if style_file is not None:
        args += ["-assume-filename", str(style_file)]
    else:
        args += ["-assume-filename", "flox_capi.h"]

    proc = subprocess.run(
        args, input=text, capture_output=True, text=True, check=True
    )
    return proc.stdout


def emit(module: ir.Module, *, format: bool = True,
         style_file: Optional[Path] = None) -> str:
    """Render `module` as a complete C header string.

    Order: handles → event-data structs → function-pointer typedefs →
    callback-bundle structs → functions. The chain mirrors the dependency
    graph: event data → fnptr (uses event data) → callback bundle (uses fnptr).

    When `format=True` (the default), pipe the output through clang-format.
    """
    out = StringIO()
    out.write(HEADER_PROLOGUE)

    fp_names = {fp.name for fp in module.function_pointers}
    event_data, bundles = _partition_structs(module.structs, fp_names)

    _emit_handles(out, module.handles)
    _emit_structs(out, event_data)
    _emit_function_pointers(out, module.function_pointers)
    if bundles:
        _emit_structs(out, bundles, banner="Callback bundles")

    grouped = module.functions_by_group()
    for g in _ordered_groups(grouped):
        title = GROUP_TITLES.get(g, g.replace("_", " ").title())
        out.write(_banner(title))
        out.write("\n")
        for fn in grouped[g]:
            _emit_function(out, fn)
        out.write("\n")

    out.write(HEADER_EPILOGUE)
    text = out.getvalue()

    if format:
        text = _run_clang_format(text, style_file=style_file)
    return text
