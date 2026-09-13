"""python/tests/test_array_length_safety.py

Regression tests for a single defect class across the pybind11 binding
layer: several numpy-array-taking functions either (a) read their
element count from one input array and never checked the others
actually matched, or (b) declared the numpy argument as a bare
``py::array_t<T>`` without ``py::array::c_style``, so a non-contiguous
view (a ``[::2]`` slice, a field of a structured array, a 2D column)
was accepted and walked with a stride-1 assumption instead of being
copied into a contiguous buffer first.

Both bugs are silent by default: mismatched lengths read past the end
of the shorter array (denormals, then a crash once the read walks off
a page); a strided view under stride-1 iteration produces numbers that
look plausible but are not the ones the caller passed in. The fix
applied throughout ``python/*.h`` is the pattern already used by
``rawToDouble``/``whites_reality_check`` before this change: require
``c_style | forcecast`` on every array parameter, and check lengths
against each other before touching any of them.

Run from repo root:
    PYTHONPATH=build/python python3 -m pytest python/tests/test_array_length_safety.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

import flox_py as flox  # noqa: E402
from flox_py import _flox_py as _core  # noqa: E402


def _strided_int64(values):
    """An int64 array that is provably non-contiguous: every value is
    padded with a throwaway neighbour, then sliced back out."""
    padded = np.empty(len(values) * 2, dtype=np.int64)
    padded[0::2] = values
    padded[1::2] = -1
    view = padded[0::2]
    assert not view.flags["C_CONTIGUOUS"]
    return view


def _strided_double(values):
    padded = np.empty(len(values) * 2, dtype=np.float64)
    padded[0::2] = values
    padded[1::2] = -999.0
    view = padded[0::2]
    assert not view.flags["C_CONTIGUOUS"]
    return view


class AggregateBarsLengthChecks(unittest.TestCase):
    """aggregate_*_bars used to take `n` from `timestamps` only
    and never checked prices/quantities/is_buy -- a caller passing a
    shorter array (a dropped-row column, a bad zip) read past its end
    silently, then crashed once the mismatch was large enough."""

    def test_mismatched_prices_length_rejected(self):
        ts = np.array([0, 1_000_000_000, 2_000_000_000], dtype=np.int64)
        px = np.array([100.0], dtype=np.float64)  # too short
        qty = np.array([1.0, 1.0, 1.0], dtype=np.float64)
        ib = np.array([1, 1, 1], dtype=np.uint8)
        with self.assertRaises(flox.FloxError):
            flox.aggregate_time_bars(ts, px, qty, ib, 1.0)

    def test_mismatched_quantities_length_rejected(self):
        ts = np.array([0, 1_000_000_000], dtype=np.int64)
        px = np.array([100.0, 101.0], dtype=np.float64)
        qty = np.array([1.0], dtype=np.float64)  # too short
        ib = np.array([1, 1], dtype=np.uint8)
        with self.assertRaises(flox.FloxError):
            flox.aggregate_time_bars(ts, px, qty, ib, 1.0)

    def test_mismatched_is_buy_length_rejected(self):
        ts = np.array([0, 1_000_000_000], dtype=np.int64)
        px = np.array([100.0, 101.0], dtype=np.float64)
        qty = np.array([1.0, 1.0], dtype=np.float64)
        ib = np.array([1], dtype=np.uint8)  # too short
        with self.assertRaises(flox.FloxError):
            flox.aggregate_time_bars(ts, px, qty, ib, 1.0)

    def test_realistic_misaligned_column_no_longer_silently_wrong(self):
        # A 3-element dropna-style resynchronization on one column
        # used to shift every close by 33x with no error and no
        # warning. With the length check this is now a raised
        # FloxError instead of silently-wrong output.
        n = 90
        ts = np.arange(n, dtype=np.int64) * 1_000_000_000
        px = np.linspace(100.0, 150.0, n)
        qty = np.ones(n)
        ib = np.ones(n, dtype=np.uint8)
        aligned = flox.aggregate_time_bars(ts, px, qty, ib, 1.0)
        self.assertGreater(len(aligned), 0)
        misaligned_px = px[:-3]
        with self.assertRaises(flox.FloxError):
            flox.aggregate_time_bars(ts, misaligned_px, qty, ib, 1.0)


class AggregateBarsStrideSafety(unittest.TestCase):
    """aggregate_*_bars accepted a strided view without a
    c_style copy, so a `[::2]` slice (or, in `dataset.py`, a structured
    array's field -- see BuildDatasetFieldView below) was walked at
    stride 1 and produced silently wrong bars."""

    def test_strided_prices_match_contiguous_copy(self):
        n = 40
        ts_src = np.empty(n * 2, dtype=np.int64)
        ts_src[0::2] = np.arange(n, dtype=np.int64) * 1_000_000_000
        ts_src[1::2] = 0
        ts = ts_src[0::2]

        px = _strided_double(np.linspace(100.0, 139.0, n))
        qty = _strided_double(np.ones(n))
        ib_src = np.empty(n * 2, dtype=np.uint8)
        ib_src[0::2] = 1
        ib = ib_src[0::2]

        strided_bars = flox.aggregate_time_bars(ts, px, qty, ib, 1.0)
        contig_bars = flox.aggregate_time_bars(
            np.ascontiguousarray(ts), np.ascontiguousarray(px),
            np.ascontiguousarray(qty), np.ascontiguousarray(ib), 1.0,
        )
        self.assertTrue(
            np.array_equal(strided_bars["close_raw"], contig_bars["close_raw"])
        )
        self.assertEqual(len(strided_bars), n)  # one trade per second, 1s bars


class BuildDatasetFieldView(unittest.TestCase):
    """The real, already-shipping consumer: `flox_py.dataset`
    passes `trades["exchange_ts_ns"]` -- a field of a structured
    array, stride == the struct size, never 8 bytes -- straight into
    `aggregate_time_bars`. Before the c_style fix this silently
    produced far more "bars" than trades and non-monotonic
    timestamps."""

    def test_bars_from_trades_field_view_are_correct(self):
        from flox_py.dataset import _bars_from_trades

        n = 40
        dtype = np.dtype([
            ("exchange_ts_ns", np.int64),
            ("price_raw", np.int64),
            ("qty_raw", np.int64),
            ("side", np.uint8),
        ])
        trades = np.zeros(n, dtype=dtype)
        trades["exchange_ts_ns"] = np.arange(n, dtype=np.int64) * 1_000_000_000
        trades["price_raw"] = np.int64(100 * flox.PRICE_SCALE)
        trades["qty_raw"] = np.int64(1 * flox.QUANTITY_SCALE)
        trades["side"] = 0  # BUY

        self.assertFalse(trades["exchange_ts_ns"].flags["C_CONTIGUOUS"])

        bars = _bars_from_trades(trades, interval_seconds=1.0)
        # One trade per second at 1s bars: exactly `n` bars, strictly
        # increasing start times -- not 199-from-10 with timestamps
        # jumping between 2023 and 1972, measured by hand against the unfixed binding.
        self.assertEqual(len(bars), n)
        starts = bars["start_time_ns"]
        self.assertTrue(np.all(np.diff(starts) > 0))


class BarReturnsAndTradePnlLengthChecks(unittest.TestCase):
    """bar_returns/trade_pnl took `n` from signal_long only."""

    def test_bar_returns_mismatched_signal_short_rejected(self):
        signal_long = np.ones(10, dtype=np.int8)
        signal_short = np.zeros(2, dtype=np.int8)  # too short
        log_returns = np.zeros(10, dtype=np.float64)
        with self.assertRaises(flox.FloxError):
            flox.bar_returns(signal_long, signal_short, log_returns)

    def test_bar_returns_mismatched_log_returns_rejected(self):
        signal_long = np.ones(10, dtype=np.int8)
        signal_short = np.zeros(10, dtype=np.int8)
        log_returns = np.zeros(2, dtype=np.float64)  # too short
        with self.assertRaises(flox.FloxError):
            flox.bar_returns(signal_long, signal_short, log_returns)

    def test_trade_pnl_mismatched_length_rejected(self):
        signal_long = np.ones(10, dtype=np.int8)
        signal_short = np.zeros(3, dtype=np.int8)  # too short
        log_returns = np.zeros(10, dtype=np.float64)
        with self.assertRaises(flox.FloxError):
            flox.trade_pnl(signal_long, signal_short, log_returns)

    def test_bar_returns_empty_input_returns_empty(self):
        empty_i8 = np.array([], dtype=np.int8)
        empty_f8 = np.array([], dtype=np.float64)
        out = flox.bar_returns(empty_i8, empty_i8, empty_f8)
        self.assertEqual(len(out), 0)

    def test_bar_returns_matches_same_length_inputs(self):
        signal_long = np.array([0, 1, 1, 0, 0], dtype=np.int8)
        signal_short = np.array([0, 0, 0, 0, 1], dtype=np.int8)
        log_returns = np.array([0.0, 0.01, 0.02, -0.01, 0.03])
        out = flox.bar_returns(signal_long, signal_short, log_returns)
        self.assertEqual(len(out), 5)


class OptimizerStatsStrideSafety(unittest.TestCase):
    """correlation/bootstrap_ci/permutation_test accepted a
    strided view without a c_style copy. On a differentiating dataset
    (not a random walk, where the bug happens not to change the
    answer) this silently flips the sign of a correlation, inflates a
    confidence interval by 50x, and turns "no effect" into "highly
    significant"."""

    def test_correlation_strided_matches_contiguous(self):
        rng = np.random.default_rng(7)
        m = rng.normal(size=(2000, 2))
        x = np.ascontiguousarray(m[:, 0])
        y_strided = m[:, 1]  # 2D column -- classically non-contiguous
        self.assertFalse(y_strided.flags["C_CONTIGUOUS"])
        y_contig = np.ascontiguousarray(y_strided)

        strided_result = flox.correlation(x, y_strided)
        contig_result = flox.correlation(x, y_contig)
        self.assertAlmostEqual(strided_result, contig_result, places=9)

    def test_bootstrap_ci_strided_matches_contiguous(self):
        rng = np.random.default_rng(11)
        data = rng.normal(loc=10.0, scale=5.0, size=800)
        strided = data[::2]
        self.assertFalse(strided.flags["C_CONTIGUOUS"])
        contig = np.ascontiguousarray(strided)

        lo_s, med_s, hi_s = flox.bootstrap_ci(strided, 0.95, 5000)
        lo_c, med_c, hi_c = flox.bootstrap_ci(contig, 0.95, 5000)
        self.assertAlmostEqual(lo_s, lo_c, places=6)
        self.assertAlmostEqual(med_s, med_c, places=6)
        self.assertAlmostEqual(hi_s, hi_c, places=6)

    def test_permutation_test_strided_matches_contiguous(self):
        rng = np.random.default_rng(13)
        g1 = rng.normal(loc=0.0, size=800)
        g2 = rng.normal(loc=0.5, size=800)
        g1_strided = g1[::2]
        self.assertFalse(g1_strided.flags["C_CONTIGUOUS"])
        g1_contig = np.ascontiguousarray(g1_strided)

        p_strided = flox.permutation_test(g1_strided, g2[:400], 2000)
        p_contig = flox.permutation_test(g1_contig, g2[:400], 2000)
        self.assertAlmostEqual(p_strided, p_contig, places=6)


class DataWriterStrideSafety(unittest.TestCase):
    """DataWriter.write_trades checked
    every array's length but not its contiguity, so a strided price or
    quantity column was written to disk wrong -- a durable corruption
    (the tape looks fine until replayed) rather than a one-off bad
    read."""

    def test_write_trades_strided_round_trips_correctly(self):
        import tempfile

        n = 20
        ts = np.arange(n, dtype=np.int64) * 1_000_000_000
        recv = ts.copy()
        px_all = np.empty(n * 2)
        px_all[0::2] = np.linspace(100.0, 119.0, n)
        px_all[1::2] = -1.0
        px = px_all[0::2]
        self.assertFalse(px.flags["C_CONTIGUOUS"])
        qty = np.ones(n)
        trade_ids = np.arange(n, dtype=np.uint64)
        symbol_ids = np.ones(n, dtype=np.uint32)
        sides = np.zeros(n, dtype=np.uint8)

        with tempfile.TemporaryDirectory() as tmp:
            writer = _core.DataWriter(tmp, 256, 0, "none")
            written = writer.write_trades(ts, recv, px, qty, trade_ids, symbol_ids, sides)
            writer.close()
            self.assertEqual(written, n)

            reader = _core.DataReader(tmp)
            rows = reader.read_trades()
            self.assertEqual(len(rows), n)
            recorded_px = rows["price_raw"].astype(np.float64) / float(_core.PRICE_SCALE)
            self.assertTrue(np.allclose(np.sort(recorded_px), np.sort(px)))


class ProfileAddTradesStrideSafety(unittest.TestCase):
    """VolumeProfile/MarketProfile/
    FootprintBar.add_trades had the length check but not the c_style
    flag."""

    def test_volume_profile_strided_matches_contiguous(self):
        n = 30
        px = _strided_double(np.linspace(100.0, 100.29, n))
        qty = _strided_double(np.ones(n))
        ib_all = np.empty(n * 2, dtype=np.uint8)
        ib_all[0::2] = 1
        ib = ib_all[0::2]

        vp_strided = flox.VolumeProfile(0.01)
        vp_strided.add_trades(px, qty, ib)
        vp_contig = flox.VolumeProfile(0.01)
        vp_contig.add_trades(
            np.ascontiguousarray(px), np.ascontiguousarray(qty), np.ascontiguousarray(ib)
        )
        self.assertAlmostEqual(vp_strided.total_volume(), vp_contig.total_volume(), places=6)
        self.assertAlmostEqual(vp_strided.poc(), vp_contig.poc(), places=6)

    def test_footprint_bar_strided_matches_contiguous(self):
        n = 30
        px = _strided_double(np.linspace(100.0, 100.29, n))
        qty = _strided_double(np.ones(n))
        ib_all = np.empty(n * 2, dtype=np.uint8)
        ib_all[0::2] = 1
        ib = ib_all[0::2]

        fp_strided = flox.FootprintBar(0.01)
        fp_strided.add_trades(px, qty, ib)
        fp_contig = flox.FootprintBar(0.01)
        fp_contig.add_trades(
            np.ascontiguousarray(px), np.ascontiguousarray(qty), np.ascontiguousarray(ib)
        )
        self.assertAlmostEqual(fp_strided.total_volume(), fp_contig.total_volume(), places=6)


class BookUpdateLengthAndStrideSafety(unittest.TestCase):
    """PyOrderBook/PyCompositeBookMatrix
    read `nb`/`na` from bid_px/ask_px only and never checked that
    bid_qty/ask_qty actually had that many elements, on top of missing
    c_style."""

    def test_order_book_mismatched_bid_qty_rejected(self):
        book = flox.OrderBook(0.01)
        bid_px = np.array([100.0, 99.99], dtype=np.float64)
        bid_qty = np.array([1.0], dtype=np.float64)  # too short
        ask_px = np.array([100.01], dtype=np.float64)
        ask_qty = np.array([1.0], dtype=np.float64)
        with self.assertRaises(flox.FloxError):
            book.apply_snapshot(bid_px, bid_qty, ask_px, ask_qty)

    def test_order_book_mismatched_ask_qty_rejected(self):
        book = flox.OrderBook(0.01)
        bid_px = np.array([100.0], dtype=np.float64)
        bid_qty = np.array([1.0], dtype=np.float64)
        ask_px = np.array([100.01, 100.02], dtype=np.float64)
        ask_qty = np.array([1.0], dtype=np.float64)  # too short
        with self.assertRaises(flox.FloxError):
            book.apply_snapshot(bid_px, bid_qty, ask_px, ask_qty)

    def test_order_book_strided_snapshot_matches_contiguous(self):
        bid_px = _strided_double([100.0, 99.99, 99.98])
        bid_qty = _strided_double([1.0, 2.0, 3.0])
        ask_px = _strided_double([100.01, 100.02])
        ask_qty = _strided_double([1.5, 2.5])

        strided_book = flox.OrderBook(0.01)
        strided_book.apply_snapshot(bid_px, bid_qty, ask_px, ask_qty)
        contig_book = flox.OrderBook(0.01)
        contig_book.apply_snapshot(
            np.ascontiguousarray(bid_px), np.ascontiguousarray(bid_qty),
            np.ascontiguousarray(ask_px), np.ascontiguousarray(ask_qty),
        )
        self.assertEqual(strided_book.best_bid(), contig_book.best_bid())
        self.assertEqual(strided_book.best_ask(), contig_book.best_ask())

    def test_composite_book_matrix_mismatched_qty_rejected(self):
        matrix = flox.CompositeBookMatrix(5000)
        bid_px = np.array([100.0, 99.99], dtype=np.float64)
        bid_qty = np.array([1.0], dtype=np.float64)  # too short
        ask_px = np.array([100.01], dtype=np.float64)
        ask_qty = np.array([1.0], dtype=np.float64)
        with self.assertRaises(flox.FloxError):
            matrix.update_book(0, 1, bid_px, bid_qty, ask_px, ask_qty, 0, False)


class LiquidationEngineContractMultiplier(unittest.TestCase):
    """LiquidationEngine.open_position did not accept
    contract_multiplier, so an orphan position opened through Python
    always looked like a 1x perp -- the contract-multiplier scaling
    the engine's realized-loss calculation already applies was
    unreachable from this path for options/futures with a multiplier
    other than 1. Account.open_position already exposed it; this just
    brings LiquidationEngine.open_position to the same surface."""

    def test_open_position_accepts_contract_multiplier(self):
        engine = flox.LiquidationEngine()
        engine.add_tier(0.0, 0.05)
        # Passing a non-default multiplier must not raise -- this was
        # a TypeError (unexpected keyword) before the fix.
        engine.open_position(
            account_id=1, symbol=1, quantity=1.0, entry_price=100.0,
            equity=10.0, contract_multiplier=100.0, is_long_option=False,
        )
        self.assertEqual(engine.position_count(), 1)

    def test_open_position_default_multiplier_is_one(self):
        engine = flox.LiquidationEngine()
        engine.add_tier(0.0, 0.05)
        # No contract_multiplier given -- must still work exactly as
        # before (default 1.0), so existing callers are unaffected.
        engine.open_position(account_id=1, symbol=1, quantity=1.0,
                             entry_price=100.0, equity=10.0)
        self.assertEqual(engine.position_count(), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
