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

    def test_analyze_path_on_non_utf8_file_does_not_raise(self) -> None:
        # analyze_source catches a SyntaxError so a lint
        # pipeline over many files does not abort on one bad file;
        # analyze_path used to have no equivalent guard and raised
        # UnicodeDecodeError straight out of the CLI on the first
        # non-UTF-8 strategy file.
        import tempfile
        fd, name = tempfile.mkstemp(suffix=".py")
        try:
            with open(fd, "wb") as f:
                f.write("# coment\xe1rio\nx = 1\n".encode("latin-1"))
            report = lookahead.analyze_path(name)
            self.assertFalse(report.ok)
            self.assertEqual([f.rule for f in report.findings], ["read_error"])
        finally:
            Path(name).unlink()

    def test_analyze_path_on_missing_file_does_not_raise(self) -> None:
        report = lookahead.analyze_path("/nonexistent/path/strategy.py")
        self.assertFalse(report.ok)
        self.assertEqual([f.rule for f in report.findings], ["read_error"])


class MissedFormsTests(unittest.TestCase):
    """forms the detector's report claimed were out of scope
    (or, for `np.roll`, claimed WERE caught) but were actually missed
    entirely."""

    def test_shift_keyword_periods_is_flagged(self) -> None:
        report = lookahead.analyze_source("df['close'].shift(periods=-1)\n")
        self.assertIn("shift_negative", [f.rule for f in report.findings])

    def test_shift_negative_via_literal_starred_dict_is_flagged(self) -> None:
        report = lookahead.analyze_source("df['close'].shift(**{'periods': -1})\n")
        self.assertIn("shift_negative", [f.rule for f in report.findings])

    def test_numpy_roll_negative_is_flagged(self) -> None:
        # docs/how-to/lookahead-detector.md claims this is caught;
        # before the fix it was not.
        report = lookahead.analyze_source("next_close = np.roll(close, -1)\n")
        self.assertIn("shift_negative", [f.rule for f in report.findings])

    def test_numpy_roll_positive_is_clean(self) -> None:
        report = lookahead.analyze_source("prev_close = np.roll(close, 1)\n")
        self.assertEqual([f.rule for f in report.findings], [])

    def test_iloc_literal_on_left_is_flagged(self) -> None:
        report = lookahead.analyze_source("future = df.iloc[1 + i]\n")
        self.assertIn("forward_index_add", [f.rule for f in report.findings])

    def test_iloc_tuple_index_is_flagged(self) -> None:
        report = lookahead.analyze_source("future = df.iloc[i + 1, 0]\n")
        self.assertIn("forward_index_add", [f.rule for f in report.findings])

    def test_forward_slice_close_is_flagged(self) -> None:
        report = lookahead.analyze_source("future_window = close[i:i+3]\n")
        self.assertIn("forward_slice", [f.rule for f in report.findings])

    def test_forward_slice_iloc_is_flagged(self) -> None:
        report = lookahead.analyze_source("future_window = df.iloc[i:i+3]\n")
        self.assertIn("forward_slice", [f.rule for f in report.findings])

    def test_forward_index_write_target_is_not_flagged(self) -> None:
        # A write is not a lookahead read.
        report = lookahead.analyze_source("out[i + 1] = v\n")
        self.assertEqual([f.rule for f in report.findings], [])

    def test_ring_buffer_front_trim_is_not_flagged(self) -> None:
        report = lookahead.analyze_source(
            "def on_trade(self, ctx, trade):\n"
            "    self.buf = self.buf[1:]\n"
        )
        self.assertEqual([f.rule for f in report.findings], [])

    def test_tail_slice_in_callback_is_not_flagged(self) -> None:
        report = lookahead.analyze_source(
            "def on_trade(self, ctx, trade):\n"
            "    last20 = self.buf[-20:]\n"
        )
        self.assertEqual([f.rule for f in report.findings], [])

    def test_open_upper_slice_on_name_index_still_flagged(self) -> None:
        # Regression guard: the false-positive fix (require the lower
        # bound to be a Name) must not silence the true positive it
        # was already catching.
        report = lookahead.analyze_source(
            "def on_bar(self, ctx, bar):\n"
            "    future = bar.history[i:]\n"
        )
        self.assertIn("open_upper_slice_in_callback", [f.rule for f in report.findings])


if __name__ == "__main__":
    unittest.main()
