"""python/tests/test_binding_docs_contract.py

Two documentation claims that the code contradicts, checked against the
files that carry them.

1. ``docs/bindings/README.md`` opens with "All bindings expose the same
   core model ... The API shape is the same across languages." It is not
   the same: the QuickJS binding has no ``BacktestRunner`` at all -- the
   event-driven ``run_csv`` / ``run_tape`` / ``run_ohlcv`` / ``run_bars``
   entry points the same page names as the primary path are unreachable
   from the embedded engine, which only has the signal-list ``Engine``.
   The page must carry a per-binding table instead, and that table must
   say ``BacktestRunner`` is absent in QuickJS.

   The contract this test enforces, so it can be satisfied deliberately:

   * ``docs/bindings/README.md`` contains a markdown table whose header
     row names Python, Node, Codon and the embedded JavaScript binding
     (a header cell containing "QuickJS" or "JavaScript").
   * The table has a row whose first cell names ``BacktestRunner``.
   * In that row, the embedded-JavaScript cell is an "absent" marker
     (``no``, ``-``, ``--``, an em dash, or ``n/a``) and the Python and
     Node cells are "present" markers (``yes`` or ``y``).
   * The blanket sentence "The API shape is the same across languages."
     is gone.

   This lives here rather than in ``scripts/check_binding_parity.py``
   because that gate is driven by ``tools/codegen/binding_parity.yaml``
   and asserts that a *symbol* exists in a binding's generated surface.
   Nothing in it reads prose, and the claim under test is prose. A later
   change could cross-check the table against the same IDL; that is a
   different gate and it would still not notice the sentence.

2. ``FloxBookSnapshot.bid_qty_raw`` / ``ask_qty_raw`` are filled from the
   book where the book can answer, and 0 otherwise. The struct in
   ``include/flox/capi/flox_capi_spec.hpp`` documented only the two price
   fields ("or 0 if absent") and left the two size fields bare, so a
   direct C-ABI or Codon consumer had nothing telling it what a 0 there
   meant. The size fields must carry that note too, in the words
   "0 when the book cannot say".

Run from repo root:
    python3 -m pytest python/tests/test_binding_docs_contract.py
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BINDINGS_README = REPO_ROOT / "docs" / "bindings" / "README.md"
CAPI_SPEC = REPO_ROOT / "include" / "flox" / "capi" / "flox_capi_spec.hpp"

PRESENT_MARKERS = {"yes", "y"}
ABSENT_MARKERS = {"no", "n/a", "na", "-", "--", "—", "–", "none", "absent"}


def _tables(markdown: str) -> list[list[list[str]]]:
    """Every markdown table in the document, as a list of cell rows.

    The separator row (``|---|---|``) is dropped; the first row that
    survives is the header.
    """
    tables: list[list[list[str]]] = []
    current: list[list[str]] = []
    for line in markdown.splitlines():
        stripped = line.strip()
        if stripped.startswith("|") and stripped.endswith("|"):
            cells = [c.strip() for c in stripped.strip("|").split("|")]
            if all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c):
                continue
            current.append(cells)
            continue
        if current:
            tables.append(current)
            current = []
    if current:
        tables.append(current)
    return tables


def _column_index(header: list[str], *needles: str) -> int | None:
    for i, cell in enumerate(header):
        low = cell.lower()
        if any(n in low for n in needles):
            return i
    return None


class BindingCapabilityTable(unittest.TestCase):
    def setUp(self) -> None:
        self.text = BINDINGS_README.read_text(encoding="utf-8")

    def test_capability_table_exists(self) -> None:
        self.assertIsNotNone(self._capability_table(),
                             "docs/bindings/README.md carries no per-binding capability table "
                             "naming Python, Node, Codon and the embedded JavaScript binding")

    def test_backtest_runner_is_marked_absent_for_quickjs(self) -> None:
        table = self._capability_table()
        self.assertIsNotNone(table, "no per-binding capability table to read")
        header = table[0]
        js_col = _column_index(header, "quickjs", "javascript")
        py_col = _column_index(header, "python")
        node_col = _column_index(header, "node")

        row = None
        for cells in table[1:]:
            if cells and "backtestrunner" in cells[0].lower().replace(" ", ""):
                row = cells
                break
        self.assertIsNotNone(row, "the capability table has no BacktestRunner row")

        def marker(cells: list[str], col: int) -> str:
            return re.sub(r"[`*]", "", cells[col]).strip().lower()

        self.assertIn(marker(row, js_col), ABSENT_MARKERS,
                      "QuickJS has no BacktestRunner; the table must say so")
        self.assertIn(marker(row, py_col), PRESENT_MARKERS,
                      "Python does have BacktestRunner")
        self.assertIn(marker(row, node_col), PRESENT_MARKERS,
                      "Node does have BacktestRunner")

    def test_blanket_sameness_claim_is_gone(self) -> None:
        self.assertNotIn(
            "The API shape is the same across languages.", self.text,
            "the API shape is not the same across languages -- QuickJS has no BacktestRunner")

    def _capability_table(self):
        for table in _tables(self.text):
            if len(table) < 2:
                continue
            header = table[0]
            if (_column_index(header, "python") is not None
                    and _column_index(header, "node") is not None
                    and _column_index(header, "codon") is not None
                    and _column_index(header, "quickjs", "javascript") is not None):
                return table
        return None


class BookSnapshotSizeFieldsAreDocumented(unittest.TestCase):
    def setUp(self) -> None:
        text = CAPI_SPEC.read_text(encoding="utf-8")
        start = text.find("int64_t bid_price_raw;")
        end = text.find("} FloxBookSnapshot;", start)
        self.assertNotEqual(start, -1, "FloxBookSnapshot not found in the C API spec")
        self.assertNotEqual(end, -1, "FloxBookSnapshot has no closing brace in the C API spec")
        self.block = text[start:end]

    def test_size_fields_carry_a_comment(self) -> None:
        for field in ("bid_qty_raw", "ask_qty_raw"):
            line = next(ln for ln in self.block.splitlines() if field in ln)
            self.assertIn("//", line,
                          f"{field} is undocumented, so a reader cannot tell what a 0 means")

    def test_size_fields_say_what_zero_means(self) -> None:
        self.assertIn("cannot say", self.block.lower(),
                      "the size fields must document 0 as \"0 when the book cannot say\"")


if __name__ == "__main__":
    unittest.main()
