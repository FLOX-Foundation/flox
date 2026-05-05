"""Tests for ``flox_py.report.write_html``.

Render the HTML report with synthetic stats / equity / trades and
assert the file is non-empty, self-contained (no external assets),
and that the key metrics show up where expected.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

from flox_py.report import render_html, write_html  # noqa: E402


_STATS = {
    "total_trades": 187,
    "winning_trades": 124,
    "losing_trades": 63,
    "initial_capital": 10000.0,
    "final_capital": 9879.0,
    "net_pnl": -121.0,
    "total_fees": 8.4,
    "max_drawdown_pct": 1.503,
    "win_rate": 0.663,
    "sharpe": -4.5746,
    "profit_factor": 0.92,
    "return_pct": -1.2103,
}

_EQUITY = {
    "timestamp_ns": [1_700_000_000_000_000_000 + i * 60_000_000_000
                     for i in range(200)],
    "equity": [10000.0 - 0.6 * i + (i % 7) * 0.3 for i in range(200)],
    "drawdown_pct": [(0.6 * i / 10000.0) * 100 for i in range(200)],
}

_TRADES = {
    "symbol": [1] * 5,
    "side": [0, 1, 0, 1, 0],
    "entry_price": [100.0, 101.5, 99.0, 100.7, 99.5],
    "exit_price": [100.5, 99.0, 100.0, 99.5, 100.5],
    "quantity": [0.01] * 5,
    "pnl": [0.5, 2.5, 1.0, 1.2, 1.0],
    "fee": [0.04, 0.04, 0.04, 0.04, 0.04],
    "entry_time_ns": [1_700_000_000_000_000_000] * 5,
    "exit_time_ns": [1_700_000_001_000_000_000] * 5,
}


class ReportHtmlTests(unittest.TestCase):

    def test_render_html_is_non_empty(self):
        html = render_html(_STATS, equity_curve=_EQUITY, trades=_TRADES)
        self.assertGreater(len(html), 1000)

    def test_no_external_assets(self):
        # The whole point of self-contained — no CDN, no <script src=...>,
        # no external <link>. Inline styles are the one form of <link rel>
        # that's OK (we don't emit those anyway).
        html = render_html(_STATS, equity_curve=_EQUITY, trades=_TRADES)
        self.assertNotIn("<script src=", html)
        self.assertNotIn("<link rel=\"stylesheet\"", html)
        self.assertNotIn("https://cdn", html)
        self.assertNotIn("http://cdn", html)

    def test_summary_metrics_present(self):
        html = render_html(_STATS, equity_curve=_EQUITY, trades=_TRADES)
        # Return is highlighted; sharpe and trades should be there too.
        self.assertIn("-1.2103", html)  # return_pct
        self.assertIn("-4.5746", html)  # sharpe
        self.assertIn("187", html)      # total_trades
        self.assertIn("66.30%", html)   # win_rate formatted

    def test_equity_curve_drawn_as_svg(self):
        html = render_html(_STATS, equity_curve=_EQUITY, trades=_TRADES)
        # Equity polyline + drawdown polyline should both be present.
        self.assertEqual(html.count("<polyline"), 2)
        self.assertIn("Equity over time", html)
        self.assertIn("Drawdown %", html)

    def test_trades_table_rows(self):
        html = render_html(_STATS, equity_curve=_EQUITY, trades=_TRADES)
        # 5 trade rows + 1 header row → 6 <tr>.
        self.assertEqual(html.count("<tr"), 6)
        self.assertIn("long", html)
        self.assertIn("short", html)

    def test_render_without_equity_or_trades_still_produces_summary(self):
        html = render_html(_STATS)
        # Cards still render even without time-series data.
        self.assertIn("-1.2103", html)
        self.assertIn("no equity data", html)
        self.assertIn("no trades data", html)

    def test_write_html_creates_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "report.html"
            write_html(out, stats=_STATS, equity_curve=_EQUITY, trades=_TRADES)
            self.assertTrue(out.exists())
            self.assertGreater(out.stat().st_size, 1000)

    def test_html_escape_in_title(self):
        html = render_html(
            _STATS, title="<script>alert(1)</script>",
            subtitle="A & B")
        # Title should be escaped.
        self.assertNotIn("<script>alert(1)</script>", html)
        self.assertIn("&lt;script&gt;", html)
        self.assertIn("A &amp; B", html)


if __name__ == "__main__":
    unittest.main(verbosity=2)
