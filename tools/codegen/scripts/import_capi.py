#!/usr/bin/env python3
"""One-shot importer: convert hand-written flox_capi.h into an annotated spec.

Reads include/flox/capi/flox_capi.h, infers a section banner ("group=") for
each declaration based on the nearest preceding `// === ... ===` comment block,
and writes a complete flox_capi_spec.hpp with FLOX_EXPORT annotations on every
function declaration.

This is intentionally not a round-trippable import — it is run once at T014
to bootstrap the full-coverage spec. After that, the spec is the source of
truth and changes go there directly.

Usage:
    PYTHONPATH=tools/codegen tools/codegen/.venv/bin/python \
        tools/codegen/scripts/import_capi.py \
        --capi include/flox/capi/flox_capi.h \
        --out  include/flox/capi/flox_capi_spec.hpp
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Relative import from the package — the script runs with PYTHONPATH=tools/codegen.
from flox_codegen import extractor


# Banners look like one of:
#   // ============================================================
#   // Order book
#   // ============================================================
# OR with extended doc body inside the banner:
#   // ============================================================
#   // Targets (forward-looking labels, batch only)
#   //
#   // Targets read into the future relative to t. They are intentionally
#   // separate from indicators: ...
#   // ============================================================
# We match the opening `===` line, capture the very first non-`===` comment
# line as the title, and then consume any number of additional comment-only
# lines until the closing `===` line. The closing line is non-greedy so
# nested banners don't fold into one.
_BANNER_RE = re.compile(
    r"""
    ^\s*//\s*=+\s*$\n           # opening === line
    ^\s*//\s*(?P<title>[^\n=]+?)\s*$\n   # title line (first non-= comment)
    (?:^\s*//[^\n]*\n)*?        # zero or more inner comment lines (non-greedy)
    ^\s*//\s*=+\s*$             # closing === line
    """,
    re.MULTILINE | re.VERBOSE,
)


def _slugify_banner(title: str) -> str:
    """Turn 'Symbol registry' or 'Order book level access' into 'registry'.

    We pick the first 1-2 *significant* words — small enough to fit in
    annotations, descriptive enough to group meaningfully. The full title
    is preserved in the source via comment passthrough; the slug is just
    a key for the IR.
    """
    # Strip parenthetical clauses ("(returns OrderId, 0 on failure)") first.
    title = re.sub(r"\(.+?\)", "", title).strip()
    # Common stopwords we drop entirely.
    STOP = {"the", "for", "and", "of", "an", "a", "ops", "operations",
            "events", "tracking", "type", "types", "level", "access"}
    words = [w.lower() for w in re.split(r"[^A-Za-z0-9]+", title) if w]
    significant = [w for w in words if w not in STOP] or words
    return "_".join(significant[:2])


def _scan_banners(text: str) -> List[Tuple[int, str]]:
    """Return [(byte_offset, slug), ...] of banner-defined sections, in order."""
    out = []
    for m in _BANNER_RE.finditer(text):
        # The slug source is the middle line.
        title = m.group(1)
        out.append((m.start(), _slugify_banner(title)))
    return out


def _line_to_offset(text: str, line_no: int) -> int:
    """Convert a 1-indexed line number to a byte offset (best effort)."""
    cur = 0
    for i, line in enumerate(text.splitlines(keepends=True), start=1):
        if i == line_no:
            return cur
        cur += len(line)
    return cur


def _group_for_function(
    line_no: int,
    text: str,
    banners: List[Tuple[int, str]],
) -> str:
    """Find the most recent banner whose offset precedes the function line."""
    fn_off = _line_to_offset(text, line_no)
    best = "_ungrouped"
    for off, slug in banners:
        if off < fn_off:
            best = slug
        else:
            break
    return best


@dataclass
class CapiDecl:
    """One declaration as captured from flox_capi.h via libclang."""

    kind: str  # "handle" | "struct" | "fnptr" | "enum" | "function"
    name: str
    line: int
    text: str  # exact source text — preserves newlines, comments adjacent
    group: str = ""  # filled in for functions only


def _extract_capi(path: Path) -> Tuple[List[CapiDecl], str]:
    """Use libclang to enumerate every top-level declaration in `path`.

    Returns (decls, source_text). decls are sorted by source line.
    """
    text = path.read_text()
    extractor._ensure_libclang_loaded()
    import clang.cindex
    K = clang.cindex.CursorKind

    args = ["-x", "c++", "-std=c++23"]
    # Engine include root, in case the header transitively pulls anything in.
    args += ["-I", str(path.parent.parent.parent)]
    for d in extractor._discover_system_includes():
        args += ["-I", d]

    index = clang.cindex.Index.create()
    tu = index.parse(str(path), args=args, options=0)
    diags = [
        d for d in tu.diagnostics
        if d.severity >= clang.cindex.Diagnostic.Error
    ]
    if diags:
        msg = "\n".join(f"  {d.location}: {d.spelling}" for d in diags)
        raise RuntimeError(f"libclang errors parsing {path}:\n{msg}")

    decls: List[CapiDecl] = []
    spec_path_str = str(path)

    def visit(c):
        if c.kind in (K.NAMESPACE, K.UNEXPOSED_DECL, K.LINKAGE_SPEC):
            for ch in c.get_children():
                visit(ch)
            return
        loc = c.location
        if loc.file is None or loc.file.name != spec_path_str:
            return
        line = loc.line

        if c.kind == K.FUNCTION_DECL:
            name = c.spelling
            decls.append(CapiDecl(kind="function", name=name, line=line, text=""))
            return
        if c.kind == K.TYPEDEF_DECL:
            underlying = c.underlying_typedef_type.spelling
            name = c.spelling
            if underlying == "void *":
                decls.append(CapiDecl(kind="handle", name=name, line=line, text=""))
            elif "(*)" in underlying:
                decls.append(CapiDecl(kind="fnptr", name=name, line=line, text=""))
            return
        if c.kind == K.STRUCT_DECL and c.spelling:
            decls.append(CapiDecl(kind="struct", name=c.spelling, line=line, text=""))
            return
        if c.kind == K.ENUM_DECL and c.spelling:
            decls.append(CapiDecl(kind="enum", name=c.spelling, line=line, text=""))
            return

    for ch in tu.cursor.get_children():
        visit(ch)

    decls.sort(key=lambda d: d.line)
    return decls, text


def _slice_decl_text(text: str, decls: List[CapiDecl], i: int) -> str:
    """Extract the source-text span for declaration `i`.

    We slice from the start of the decl's line to the start of the next decl
    (or the closing `}` of the extern "C" block, whichever is earlier).
    Inline comments adjacent to the decl get carried over.
    """
    lines = text.splitlines(keepends=True)
    start_line = decls[i].line
    if i + 1 < len(decls):
        end_line = decls[i + 1].line
    else:
        end_line = len(lines) + 1

    return "".join(lines[start_line - 1: end_line - 1])


def import_capi(capi_path: Path, out_path: Path) -> None:
    """Convert capi_path into a fully-annotated spec written to out_path."""
    decls, text = _extract_capi(capi_path)
    banners = _scan_banners(text)

    # Decorate functions with a group slug.
    for d in decls:
        if d.kind == "function":
            d.group = _group_for_function(d.line, text, banners)

    # Emit the spec. We rebuild the file from scratch, preserving:
    # - The original banner sections (verbatim comments).
    # - Each declaration's source text (verbatim).
    # - FLOX_EXPORT annotations on every function declaration.
    out_lines: List[str] = []
    out_lines.append("/*\n")
    out_lines.append(" * Flox Engine\n")
    out_lines.append(" * Developed by FLOX Foundation"
                    " (https://github.com/FLOX-Foundation)\n")
    out_lines.append(" *\n")
    out_lines.append(" * Copyright (c) 2025 FLOX Foundation\n")
    out_lines.append(" * Licensed under the MIT License. See LICENSE file"
                    " in the project root for full\n")
    out_lines.append(" * license information.\n")
    out_lines.append(" */\n")
    out_lines.append("\n")
    out_lines.append("// flox_capi_spec.hpp — IDL spec for the FLOX C API.\n")
    out_lines.append("//\n")
    out_lines.append("// Generated from flox_capi.h via"
                    " tools/codegen/scripts/import_capi.py.\n")
    out_lines.append("// After bootstrap, this file is the source of truth"
                    " — edit here, regenerate\n")
    out_lines.append("// the C header via tools/codegen/scripts/regenerate.sh.\n")
    out_lines.append("\n")
    out_lines.append("#pragma once\n")
    out_lines.append("\n")
    out_lines.append("#include <stddef.h>\n")
    out_lines.append("#include <stdint.h>\n")
    out_lines.append("\n")
    out_lines.append('#include "flox/capi/flox_export.h"\n')
    out_lines.append("\n")
    out_lines.append("#ifdef __cplusplus\n")
    out_lines.append('extern "C"\n')
    out_lines.append("{\n")
    out_lines.append("#endif\n")
    out_lines.append("\n")

    # Walk decls in line order, emitting each with its banner above (when the
    # banner comes between the previous decl and this one).
    text_lines = text.splitlines(keepends=True)

    # Skip the original prologue: anything before the first declaration is
    # the existing license header + includes + extern "C" {, which our spec's
    # own prologue already provides. Banner comments preceding the first decl
    # do get carried over (they describe the first section).
    if not decls:
        out_lines.append("\n#ifdef __cplusplus\n}\n#endif\n")
        out_path.write_text("".join(out_lines))
        return

    # Find the highest "skip from" point: the line of the opening
    # `extern "C"` or `{` that ends the prologue. We want to start AFTER it,
    # but BEFORE any banner comments preceding the first decl. Heuristic:
    # rewind from the first decl back through blank/comment lines until we
    # hit non-comment code; the line after that is our start.
    first_decl_line = decls[0].line
    start_line = first_decl_line
    for j in range(first_decl_line - 1, 0, -1):
        ln = text_lines[j - 1].strip()
        if ln == "" or ln.startswith("//"):
            start_line = j
        else:
            break
    cursor_line = start_line

    for i, decl in enumerate(decls):
        # Capture intervening comments (banners + free comments) between
        # cursor_line and decl.line — pass them through verbatim.
        if cursor_line < decl.line:
            preamble = "".join(text_lines[cursor_line - 1: decl.line - 1])
            # Strip trailing blank lines that would double up at the start.
            out_lines.append(preamble)

        # Capture the decl's own source span.
        if i + 1 < len(decls):
            decl_end_line = decls[i + 1].line
        else:
            # Until the end of the extern block — use the file's closing block
            # marker if we can find it.
            close_match = None
            for ln_idx, ln in enumerate(text_lines[decl.line - 1:],
                                        start=decl.line):
                if ln.strip().startswith("#ifdef __cplusplus") and ln_idx > decl.line:
                    close_match = ln_idx
                    break
            decl_end_line = close_match if close_match else len(text_lines) + 1

        decl_text = "".join(text_lines[decl.line - 1: decl_end_line - 1])

        # Inject FLOX_EXPORT annotation on functions.
        if decl.kind == "function":
            # Find the line where the function starts in decl_text.
            decl_lines = decl_text.splitlines(keepends=True)
            # Skip leading comments (// ...) and blank lines until first
            # non-comment non-blank line.
            inject_at = 0
            for j, ln in enumerate(decl_lines):
                stripped = ln.strip()
                if not stripped:
                    continue
                if stripped.startswith("//") or stripped.startswith("/*"):
                    continue
                inject_at = j
                break
            # Inject the annotation as its own line right before the decl.
            indent = re.match(r"^( *)", decl_lines[inject_at]).group(1)
            anno_line = (
                f'{indent}FLOX_EXPORT(group = "{decl.group}")\n'
            )
            decl_lines.insert(inject_at, anno_line)
            decl_text = "".join(decl_lines)

        out_lines.append(decl_text)
        cursor_line = decl_end_line

    # Trail anything between the last decl and the closing brace.
    if cursor_line <= len(text_lines):
        # Only carry over until the existing closing `}` of the extern block,
        # which we replace with our own.
        trailing = "".join(text_lines[cursor_line - 1:])
        # Strip the original closing block; we add our own.
        trailing = re.sub(
            r"#ifdef __cplusplus\s*\n\}\s*\n#endif\s*\n?$",
            "",
            trailing,
            flags=re.MULTILINE,
        )
        out_lines.append(trailing)

    out_lines.append("\n#ifdef __cplusplus\n}\n#endif\n")

    out_path.write_text("".join(out_lines))
    print(
        f"wrote {out_path}: "
        f"{sum(1 for d in decls if d.kind == 'function')} functions, "
        f"{sum(1 for d in decls if d.kind == 'handle')} handles, "
        f"{sum(1 for d in decls if d.kind == 'struct')} structs, "
        f"{sum(1 for d in decls if d.kind == 'enum')} enums, "
        f"{sum(1 for d in decls if d.kind == 'fnptr')} fnptrs"
    )


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--capi", required=True, type=Path,
                   help="Path to existing flox_capi.h")
    p.add_argument("--out", required=True, type=Path,
                   help="Path to write spec to (overwrites)")
    args = p.parse_args(argv)
    import_capi(args.capi, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
