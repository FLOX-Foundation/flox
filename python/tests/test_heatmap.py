"""Tests for ``flox_py.report.heatmap_html`` / ``write_heatmap``.

The render lives in C++ (``flox::report::renderHeatmapHtml``); these
tests just exercise the Python wrapper, asserting the HTML structure
is well-formed and labels / metric names land where expected.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

from flox_py.report import heatmap_html, write_heatmap  # noqa: E402


_Z = [
    [0.5, -0.3, 1.2],
    [0.8, 1.1, -1.4],
]


class HeatmapRenderTests(unittest.TestCase):

    def test_basic_render_is_non_empty_html(self):
        html = heatmap_html(_Z)
        self.assertGreater(len(html), 500)
        self.assertIn("<svg", html)
        self.assertIn("</svg>", html)
        self.assertIn("<!doctype html>", html)

    def test_no_external_assets(self):
        html = heatmap_html(_Z)
        self.assertNotIn("<script src=", html)
        self.assertNotIn("https://cdn", html)
        self.assertNotIn("http://cdn", html)

    def test_labels_appear_in_output(self):
        html = heatmap_html(
            _Z,
            row_labels=["fast=5", "fast=10"],
            col_labels=["slow=20", "slow=30", "slow=50"],
            title="Sweep",
            x_axis_name="slow period",
            y_axis_name="fast period",
            metric_name="Sharpe",
        )
        self.assertIn("fast=5", html)
        self.assertIn("slow=30", html)
        self.assertIn("Sweep", html)
        self.assertIn("slow period", html)
        self.assertIn("fast period", html)
        self.assertIn("Sharpe", html)

    def test_html_escape_on_labels(self):
        html = heatmap_html(_Z, title="<script>alert(1)</script>")
        self.assertNotIn("<script>alert(1)</script>", html)
        self.assertIn("&lt;script&gt;", html)

    def test_cell_values_in_svg(self):
        html = heatmap_html(_Z)
        # 6 cells, each with a numeric label inside.
        self.assertIn("0.500", html)
        self.assertIn("-1.400", html)

    def test_uneven_rows_raises(self):
        with self.assertRaises(Exception):
            heatmap_html([[1.0, 2.0], [3.0]])

    def test_empty_z_raises(self):
        with self.assertRaises(Exception):
            heatmap_html([])

    def test_write_heatmap_creates_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "h.html"
            write_heatmap(out, _Z, title="t")
            self.assertTrue(out.exists())
            self.assertGreater(out.stat().st_size, 500)

    def test_render_byte_identical_for_same_input(self):
        # Determinism — engine renders should be reproducible.
        a = heatmap_html(_Z, title="A")
        b = heatmap_html(_Z, title="A")
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main(verbosity=2)
