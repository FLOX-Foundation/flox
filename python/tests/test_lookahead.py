"""Tests for ``flox_py.lookahead``."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

from flox_py import lookahead  # noqa: E402


class ShiftNegativeTests(unittest.TestCase):
    def test_negative_shift_is_flagged(self) -> None:
        report = lookahead.analyze_source(
            "df['close'].shift(-1)\n"
        )
        self.assertFalse(report.ok)
        rules = [f.rule for f in report.findings]
        self.assertIn("shift_negative", rules)

    def test_positive_shift_is_clean(self) -> None:
        report = lookahead.analyze_source(
            "df['close'].shift(1)\n"
        )
        self.assertEqual([f.rule for f in report.findings], [])

    def test_zero_shift_is_clean(self) -> None:
        report = lookahead.analyze_source("df.shift(0)\n")
        self.assertTrue(report.ok)


class ForwardIndexTests(unittest.TestCase):
    def test_iloc_plus_one_flagged(self) -> None:
        report = lookahead.analyze_source(
            "x = df.iloc[i + 1]\n"
        )
        self.assertIn(
            "forward_index_add",
            [f.rule for f in report.findings],
        )

    def test_array_plus_offset_flagged(self) -> None:
        report = lookahead.analyze_source("y = arr[idx + 5]\n")
        self.assertIn(
            "forward_index_add",
            [f.rule for f in report.findings],
        )

    def test_minus_offset_clean(self) -> None:
        report = lookahead.analyze_source(
            "x = df.iloc[i - 1]\n"
        )
        self.assertTrue(report.ok)


class OpenUpperSliceTests(unittest.TestCase):
    def test_open_upper_inside_callback_flagged(self) -> None:
        src = (
            "class S:\n"
            "    def on_bar(self, ctx, bar):\n"
            "        future = bar.history[i:]\n"
        )
        report = lookahead.analyze_source(src)
        self.assertIn(
            "open_upper_slice_in_callback",
            [f.rule for f in report.findings],
        )

    def test_open_upper_outside_callback_clean(self) -> None:
        src = (
            "def helper(arr, i):\n"
            "    return arr[i:]\n"
        )
        report = lookahead.analyze_source(src)
        self.assertTrue(report.ok)

    def test_closed_slice_in_callback_clean(self) -> None:
        src = (
            "class S:\n"
            "    def on_bar(self, ctx, bar):\n"
            "        past = bar.history[i - 100:i]\n"
        )
        report = lookahead.analyze_source(src)
        self.assertTrue(report.ok)


class FutureAttrTests(unittest.TestCase):
    def test_next_attr_flagged(self) -> None:
        report = lookahead.analyze_source(
            "x = trade.next_price\n"
        )
        self.assertIn(
            "future_attr_name",
            [f.rule for f in report.findings],
        )

    def test_future_attr_flagged(self) -> None:
        report = lookahead.analyze_source(
            "y = bar.future_close\n"
        )
        self.assertIn(
            "future_attr_name",
            [f.rule for f in report.findings],
        )

    def test_lookahead_attr_flagged(self) -> None:
        report = lookahead.analyze_source(
            "z = ctx.lookahead_bar\n"
        )
        self.assertIn(
            "future_attr_name",
            [f.rule for f in report.findings],
        )

    def test_unrelated_attr_clean(self) -> None:
        report = lookahead.analyze_source(
            "p = trade.price\n"
        )
        self.assertTrue(report.ok)


class CombinedTests(unittest.TestCase):
    def test_realistic_strategy_with_one_bug(self) -> None:
        src = (
            "import flox_py as flox\n"
            "\n"
            "class MyStrategy(flox.Strategy):\n"
            "    def on_bar(self, ctx, bar):\n"
            "        # bug: peeking at next bar\n"
            "        if bar.history.iloc[i + 1] > bar.close:\n"
            "            self.market_buy(0.1)\n"
        )
        report = lookahead.analyze_source(src)
        self.assertFalse(report.ok)
        rules = sorted(f.rule for f in report.findings)
        self.assertIn("forward_index_add", rules)

    def test_clean_strategy(self) -> None:
        src = (
            "import flox_py as flox\n"
            "\n"
            "class MyStrategy(flox.Strategy):\n"
            "    def __init__(self, symbols):\n"
            "        super().__init__(symbols)\n"
            "        self.fast = flox.SMA(10)\n"
            "    def on_bar(self, ctx, bar):\n"
            "        v = self.fast.update(bar.close)\n"
            "        if v is not None and bar.close > v:\n"
            "            self.market_buy(0.1)\n"
        )
        report = lookahead.analyze_source(src)
        self.assertTrue(report.ok, f"unexpected findings: {report.findings}")

    def test_syntax_error_yields_single_finding(self) -> None:
        report = lookahead.analyze_source("def x(:\n  pass\n")
        self.assertFalse(report.ok)
        self.assertEqual([f.rule for f in report.findings], ["syntax_error"])


class MCPEntryPointTests(unittest.TestCase):
    def test_validate_returns_json_with_ok_flag(self) -> None:
        out = lookahead.validate_strategy_no_lookahead(
            "x = df.shift(-1)\n"
        )
        payload = json.loads(out)
        self.assertFalse(payload["ok"])
        self.assertGreaterEqual(len(payload["findings"]), 1)


class AnalyzePathTests(unittest.TestCase):
    def test_analyze_path_reads_file(self) -> None:
        import tempfile
        f = tempfile.NamedTemporaryFile(mode="w", suffix=".py", delete=False)
        f.write("x = df.shift(-1)\n")
        f.close()
        try:
            report = lookahead.analyze_path(f.name)
            self.assertFalse(report.ok)
            self.assertEqual(report.path, f.name)
        finally:
            Path(f.name).unlink()


if __name__ == "__main__":
    unittest.main()
