#!/usr/bin/env python3
"""scripts/gen_api_index.py

Generate a complete, alphabetical Markdown index of the Python binding's
public surface from the committed `.pyi` stubs.

The hand-written narrative pages (`docs/reference/python/{engine,indicators,
backtest,...}.md`) explain *how* each piece is used. This index is the
opposite — a flat alphabetical list of every public symbol with its
exact signature, derived directly from the pyi stub. AI agents grepping
for "what's exported" land on a single page that's guaranteed in sync
with the binding (CI gate diffs the generated file against the committed
one, same pattern as `gen_indicator_docs.py`).

Output:
    docs/reference/python/_api_index.md

Usage:
    python3 scripts/gen_api_index.py            # write
    python3 scripts/gen_api_index.py --check    # exit 1 if stale
"""
from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path
from typing import List, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent
PYI = REPO_ROOT / "python" / "flox_py" / "_flox_py" / "__init__.pyi"
TARGETS_PYI = REPO_ROOT / "python" / "flox_py" / "_flox_py" / "targets.pyi"
OUT_PATH = REPO_ROOT / "docs" / "reference" / "python" / "_api_index.md"


def _format_arg(arg: ast.arg, default: ast.expr | None) -> str:
    s = arg.arg
    if arg.annotation is not None:
        s += ": " + ast.unparse(arg.annotation)
    if default is not None:
        s += " = " + ast.unparse(default)
    return s


def _format_signature(fn: ast.FunctionDef | ast.AsyncFunctionDef) -> str:
    """Render `(arg1: T1, arg2: T2 = default) -> Return` from an AST FunctionDef."""
    args = fn.args
    parts: list[str] = []

    # positional-or-keyword args + their defaults
    pos = list(args.args)
    defaults = list(args.defaults)
    n_no_default = len(pos) - len(defaults)
    for i, a in enumerate(pos):
        d = defaults[i - n_no_default] if i >= n_no_default else None
        parts.append(_format_arg(a, d))

    if args.vararg is not None:
        parts.append("*" + _format_arg(args.vararg, None))
    elif args.kwonlyargs:
        parts.append("*")

    for kwa, kwd in zip(args.kwonlyargs, args.kw_defaults):
        parts.append(_format_arg(kwa, kwd))

    if args.kwarg is not None:
        parts.append("**" + _format_arg(args.kwarg, None))

    sig = "(" + ", ".join(parts) + ")"
    if fn.returns is not None:
        sig += " -> " + ast.unparse(fn.returns)
    return sig


def _docstring_first_line(node: ast.AST) -> str:
    doc = ast.get_docstring(node)  # type: ignore[arg-type]
    if not doc:
        return ""
    line = doc.strip().splitlines()[0].strip()
    # Compact whitespace.
    return re.sub(r"\s+", " ", line)


def _is_property(fn: ast.FunctionDef) -> bool:
    for dec in fn.decorator_list:
        if isinstance(dec, ast.Name) and dec.id == "property":
            return True
        if isinstance(dec, ast.Attribute) and dec.attr in {"setter", "getter", "deleter"}:
            return True
    return False


def _format_method(fn: ast.FunctionDef) -> str:
    name = fn.name
    if _is_property(fn):
        ret = ast.unparse(fn.returns) if fn.returns is not None else "Any"
        return f"- *property* `{name}: {ret}`"
    sig = _format_signature(fn)
    return f"- `{name}{sig}`"


def _is_dunder(name: str) -> bool:
    return name.startswith("__") and name.endswith("__")


def _format_class(cls: ast.ClassDef) -> List[str]:
    out: list[str] = []
    base_repr = ", ".join(ast.unparse(b) for b in cls.bases) if cls.bases else ""
    header = f"### `class {cls.name}"
    if base_repr:
        header += f"({base_repr})"
    header += "`"
    out.append(header)
    out.append("")

    doc = _docstring_first_line(cls)
    if doc:
        out.append(doc)
        out.append("")

    method_lines: list[str] = []
    constructor_line: str | None = None
    for item in cls.body:
        if isinstance(item, ast.FunctionDef):
            if item.name == "__init__":
                sig = _format_signature(item)
                # Strip the leading `self`.
                sig = sig.replace("(self", "(", 1).replace("(, ", "(", 1)
                constructor_line = f"- `{cls.name}{sig}`"
            elif _is_dunder(item.name):
                # Skip rare dunder methods (e.g. __iter__) for readability;
                # they're listed in the stub itself if needed.
                continue
            else:
                method_lines.append(_format_method(item))
        elif isinstance(item, ast.AnnAssign) and isinstance(item.target, ast.Name):
            ann = ast.unparse(item.annotation)
            method_lines.append(f"- *attr* `{item.target.id}: {ann}`")

    if constructor_line is not None:
        out.append("**Constructor**")
        out.append("")
        out.append(constructor_line)
        out.append("")
    if method_lines:
        out.append("**Members**")
        out.append("")
        out.extend(method_lines)
        out.append("")
    return out


def _format_function(fn: ast.FunctionDef) -> str:
    sig = _format_signature(fn)
    line = f"- `{fn.name}{sig}`"
    doc = _docstring_first_line(fn)
    if doc:
        line += f" — {doc}"
    return line


def _format_constant(item: ast.AnnAssign | ast.Assign) -> str | None:
    if isinstance(item, ast.AnnAssign):
        target = item.target
        ann = ast.unparse(item.annotation)
        value = ast.unparse(item.value) if item.value is not None else "..."
    else:
        if len(item.targets) != 1:
            return None
        target = item.targets[0]
        ann = None
        value = ast.unparse(item.value)

    if not isinstance(target, ast.Name):
        return None
    name = target.id
    # Skip private + dunder + the __all__ list.
    if name.startswith("_"):
        return None
    if ann is not None:
        return f"- `{name}: {ann} = {value}`"
    return f"- `{name} = {value}`"


def _render_module(tree: ast.Module, *, title: str, fq: str) -> str:
    classes: list[ast.ClassDef] = []
    functions: list[ast.FunctionDef] = []
    constants: list[str] = []

    for item in tree.body:
        if isinstance(item, ast.ClassDef):
            if not item.name.startswith("_"):
                classes.append(item)
        elif isinstance(item, ast.FunctionDef):
            if not item.name.startswith("_"):
                functions.append(item)
        elif isinstance(item, (ast.AnnAssign, ast.Assign)):
            line = _format_constant(item)
            if line is not None:
                constants.append(line)

    classes.sort(key=lambda c: c.name)
    functions.sort(key=lambda f: f.name)

    out: list[str] = []
    out.append(f"## {title}")
    out.append("")
    out.append(f"Module: `{fq}`")
    out.append("")
    out.append(
        f"Surface: {len(classes)} classes, {len(functions)} functions, "
        f"{len(constants)} constants."
    )
    out.append("")

    if constants:
        out.append("### Constants")
        out.append("")
        out.extend(sorted(constants))
        out.append("")

    if functions:
        out.append("### Functions")
        out.append("")
        for fn in functions:
            out.append(_format_function(fn))
        out.append("")

    if classes:
        out.append("### Classes")
        out.append("")
        for cls in classes:
            out.extend(_format_class(cls))

    return "\n".join(out).rstrip() + "\n"


def _build() -> str:
    sections: list[str] = []
    sections.append(
        "# Python API index\n\n"
        "Auto-generated alphabetical index of every public symbol exported\n"
        "by the FLOX Python binding (`flox_py`). Each entry shows the exact\n"
        "signature derived from the committed `.pyi` stubs.\n\n"
        "For narrative-style \"how to use this\" docs, see the\n"
        "[Engine & Backtest](engine.md), [Indicators](indicators.md), and\n"
        "other curated reference pages. This page is the flat appendix that\n"
        "AI agents grep when searching for a specific symbol.\n\n"
        "Generated by `scripts/gen_api_index.py` from\n"
        "`python/flox_py/_flox_py/__init__.pyi`.\n"
    )

    main_tree = ast.parse(PYI.read_text())
    sections.append(_render_module(main_tree, title="flox_py", fq="flox_py"))

    if TARGETS_PYI.exists():
        targets_tree = ast.parse(TARGETS_PYI.read_text())
        sections.append(
            _render_module(targets_tree, title="flox_py.targets", fq="flox_py.targets")
        )

    return "\n".join(sections).rstrip() + "\n"


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--check", action="store_true",
                   help="don't write; exit 1 if generated content differs from committed")
    args = p.parse_args(argv)

    text = _build()
    if args.check:
        if not OUT_PATH.exists():
            print(f"::error::{OUT_PATH.relative_to(REPO_ROOT)} does not exist", file=sys.stderr)
            return 1
        committed = OUT_PATH.read_text()
        if committed != text:
            print(f"::error::{OUT_PATH.relative_to(REPO_ROOT)} is out of date "
                  "(run scripts/gen_api_index.py)", file=sys.stderr)
            return 1
        print(f"OK: {OUT_PATH.relative_to(REPO_ROOT)} in sync.")
        return 0

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(text)
    print(f"wrote {OUT_PATH.relative_to(REPO_ROOT)} ({len(text):,} chars)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
