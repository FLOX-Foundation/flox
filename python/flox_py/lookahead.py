"""Lookahead-bias detector for Python strategy code.

Lookahead bias is the most common backtest bug in algorithmic
trading: a strategy reads tomorrow's price by accident, claims a
profit it could never have realized live, and quietly fails the
moment it ships. This module catches the obvious patterns by walking
the AST. It is a heuristic, not a proof; see *What it does not
catch* at the bottom for the gap.

Patterns that fire:

* ``df.shift(-N)`` and ``Series.shift(-N)`` where ``N`` is a
  positive integer literal. Pandas / numpy shift by a negative
  offset reads from the future.
* ``df.iloc[i + N]``, ``arr[i + N]``, ``df.loc[t + delta]`` where
  ``N`` is a positive integer literal. Index-arithmetic that walks
  forward from the current bar peeks ahead.
* Subscript slices like ``df[i:]`` inside a per-bar callback when
  the upper bound is open and ``i`` is the current-bar index. (Open
  upper bounds in a per-bar context include all future rows.)
* Direct attribute access on names that look like future-dated
  fields: ``trade.next_*``, ``bar.future_*``, ``ctx.lookahead_*``.

The detector returns a list of ``Finding`` objects; ``flox lint
lookahead`` prints them, and ``validate_strategy_no_lookahead``
exposes them as an MCP tool.
"""
from __future__ import annotations

import ast
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence


# Function names that are entered when scanning hot-path callbacks.
# Any reference to a future row inside one of these is a finding.
_PER_BAR_CALLBACKS = frozenset({
    "on_trade", "on_bar", "on_book", "on_book_update",
    "on_tick", "on_quote", "signal", "should_enter", "should_exit",
    "compute", "update",
})


@dataclass
class Finding:
    """One detected lookahead pattern."""

    rule: str
    message: str
    line: int
    col: int
    snippet: str = ""

    def to_dict(self) -> dict:
        return {
            "rule": self.rule,
            "message": self.message,
            "line": self.line,
            "col": self.col,
            "snippet": self.snippet,
        }


@dataclass
class Report:
    """The full result of analyzing a source file."""

    path: Optional[str]
    findings: List[Finding] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.findings

    def to_dict(self) -> dict:
        return {
            "path": self.path,
            "ok": self.ok,
            "findings": [f.to_dict() for f in self.findings],
        }


def _is_positive_int_literal(node: ast.AST) -> Optional[int]:
    if isinstance(node, ast.Constant) and isinstance(node.value, int):
        if node.value > 0:
            return node.value
    return None


def _is_negative_int_literal(node: ast.AST) -> Optional[int]:
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        if isinstance(node.operand, ast.Constant) and isinstance(node.operand.value, int):
            if node.operand.value > 0:
                return -node.operand.value
    return None


class _Visitor(ast.NodeVisitor):
    def __init__(self, source_lines: Sequence[str]) -> None:
        self.findings: List[Finding] = []
        self._lines = list(source_lines)
        self._in_callback_depth = 0

    def _snippet(self, lineno: int) -> str:
        if 1 <= lineno <= len(self._lines):
            return self._lines[lineno - 1].strip()
        return ""

    def _add(self, rule: str, message: str, node: ast.AST) -> None:
        self.findings.append(Finding(
            rule=rule,
            message=message,
            line=getattr(node, "lineno", 0),
            col=getattr(node, "col_offset", 0),
            snippet=self._snippet(getattr(node, "lineno", 0)),
        ))

    # ── Function entry / exit (track callback depth) ─────────────

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:  # noqa: N802
        is_callback = node.name in _PER_BAR_CALLBACKS
        if is_callback:
            self._in_callback_depth += 1
        try:
            self.generic_visit(node)
        finally:
            if is_callback:
                self._in_callback_depth -= 1

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:  # noqa: N802
        # Treat the same as the sync form.
        self.visit_FunctionDef(node)  # type: ignore[arg-type]

    # ── shift(-N) on any object ──────────────────────────────────

    def visit_Call(self, node: ast.Call) -> None:  # noqa: N802
        if isinstance(node.func, ast.Attribute) and node.func.attr == "shift":
            if node.args:
                neg = _is_negative_int_literal(node.args[0])
                if neg is not None:
                    self._add(
                        "shift_negative",
                        f"`.shift({neg})` reads from the future. "
                        "Pandas / numpy shift by a negative offset "
                        "is the canonical lookahead bug.",
                        node,
                    )
        self.generic_visit(node)

    # ── df.iloc[i + N], arr[i + N], df.loc[i + N] (forward index) ─

    def visit_Subscript(self, node: ast.Subscript) -> None:  # noqa: N802
        # Detect open-upper slices like df[i:] in a per-bar callback:
        # the slice spans all future rows. Only flag inside callbacks.
        slc = node.slice
        if isinstance(slc, ast.Slice):
            if self._in_callback_depth > 0 and slc.upper is None and slc.lower is not None:
                self._add(
                    "open_upper_slice_in_callback",
                    "Open-upper slice inside a per-bar callback "
                    "includes future rows. Cap the upper bound at "
                    "the current bar index.",
                    node,
                )

        # Detect i + N where N is a positive int literal as the index
        # expression. Includes plain subscripts and BinOp Adds.
        if isinstance(slc, ast.BinOp) and isinstance(slc.op, ast.Add):
            n = _is_positive_int_literal(slc.right)
            if n is not None:
                self._add(
                    "forward_index_add",
                    f"Index `i + {n}` walks forward from the current "
                    "bar; this reads a future row.",
                    node,
                )
        self.generic_visit(node)

    # ── trade.next_*, bar.future_*, ctx.lookahead_* ──────────────

    def visit_Attribute(self, node: ast.Attribute) -> None:  # noqa: N802
        suspicious = (
            node.attr.startswith("next_")
            or node.attr.startswith("future_")
            or node.attr.startswith("lookahead_")
        )
        if suspicious:
            self._add(
                "future_attr_name",
                f"Attribute name `{node.attr}` looks like it points "
                "at a future-dated field. Verify it does not read "
                "ahead of the current bar.",
                node,
            )
        self.generic_visit(node)


def analyze_source(source: str, *, path: Optional[str] = None) -> Report:
    """Parse ``source`` and return a :class:`Report`. Syntax errors
    surface as a single ``syntax_error`` finding rather than an
    exception so the lint pipeline does not abort on a bad file."""
    try:
        tree = ast.parse(source, filename=path or "<string>")
    except SyntaxError as exc:
        return Report(
            path=path,
            findings=[Finding(
                rule="syntax_error",
                message=f"could not parse: {exc.msg}",
                line=exc.lineno or 0,
                col=exc.offset or 0,
                snippet="",
            )],
        )
    lines = source.splitlines()
    v = _Visitor(lines)
    v.visit(tree)
    return Report(path=path, findings=v.findings)


def analyze_path(path: str | Path) -> Report:
    p = Path(path).expanduser()
    return analyze_source(p.read_text(), path=str(p))


def validate_strategy_no_lookahead(code: str) -> str:
    """MCP-friendly entry point. Takes raw Python source, returns a
    JSON string with the findings. Same shape as the rest of the
    flox-mcp lint tools."""
    report = analyze_source(code)
    return json.dumps(report.to_dict(), indent=2, sort_keys=True)


__all__ = [
    "Finding",
    "Report",
    "analyze_source",
    "analyze_path",
    "validate_strategy_no_lookahead",
]
