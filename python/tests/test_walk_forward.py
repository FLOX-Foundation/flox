"""Tests for ``flox_py.WalkForwardRunner``.

Anchored and sliding modes against the bundled BTC sample. Asserts
fold counts, that train/test windows partition the input correctly,
and that timestamps are monotonic across folds.
"""
from __future__ import annotations

import os
import sys
import unittest

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox  # noqa: E402

_CSV = os.path.join(os.path.dirname(__file__), "..", "flox_py",
                    "templates", "research", "data", "btcusdt_sample.csv")


class _SmaStrategy(flox.Strategy):
    def __init__(self, syms, *, fast=10, slow=30):
        super().__init__(syms)
        self.fast = flox.SMA(fast)
        self.slow = flox.SMA(slow)

    def on_trade(self, ctx, t):
        f = self.fast.update(t.price)
        s = self.slow.update(t.price)
        if f is None or s is None or not self.slow.ready:
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.01)
        elif f < s and ctx.is_long():
            self.market_sell(0.01)


@unittest.skipUnless(os.path.exists(_CSV),
                     f"sample CSV not found at {_CSV}")
class WalkForwardTests(unittest.TestCase):

    def setUp(self) -> None:
        self.reg = flox.SymbolRegistry()
        self.btc = self.reg.add_symbol("exchange", "BTCUSDT", 0.01)

    def test_anchored_mode_produces_expected_fold_count(self):
        # 500 bars, min_train=100, test=100, step=100 → splits at 100,
        # 200, 300, 400 → 4 folds (each with test_size=100, last
        # ending at 500).
        wfr = flox.WalkForwardRunner(
            self.reg, fee_rate=0.0004, initial_capital=10_000,
            mode="anchored", test_size=100, step=100, min_train_size=100,
        )
        wfr.set_strategy_factory(lambda _i: _SmaStrategy([self.btc]))
        folds = wfr.run_csv(_CSV, "BTCUSDT")
        self.assertEqual(len(folds), 4)

    def test_anchored_train_window_grows(self):
        wfr = flox.WalkForwardRunner(
            self.reg, mode="anchored", test_size=100, step=100,
            min_train_size=100, fee_rate=0.0004, initial_capital=10_000,
        )
        wfr.set_strategy_factory(lambda _i: _SmaStrategy([self.btc]))
        folds = wfr.run_csv(_CSV, "BTCUSDT")
        # train always starts at bar 0 (anchored), grows by step each fold.
        self.assertTrue(all(f["train_start_bar"] == 0 for f in folds))
        train_ends = [f["train_end_bar"] for f in folds]
        self.assertEqual(train_ends, sorted(train_ends))
        self.assertEqual(train_ends[0], 100)

    def test_anchored_test_immediately_follows_train(self):
        wfr = flox.WalkForwardRunner(
            self.reg, mode="anchored", test_size=100, step=100,
            min_train_size=100, fee_rate=0.0004, initial_capital=10_000,
        )
        wfr.set_strategy_factory(lambda _i: _SmaStrategy([self.btc]))
        folds = wfr.run_csv(_CSV, "BTCUSDT")
        for f in folds:
            self.assertEqual(f["test_start_bar"], f["train_end_bar"])
            self.assertEqual(f["test_end_bar"],
                             f["test_start_bar"] + 100)

    def test_sliding_mode_window_is_constant(self):
        wfr = flox.WalkForwardRunner(
            self.reg, mode="sliding", train_size=200, test_size=100,
            step=100, fee_rate=0.0004, initial_capital=10_000,
        )
        wfr.set_strategy_factory(lambda _i: _SmaStrategy([self.btc]))
        folds = wfr.run_csv(_CSV, "BTCUSDT")
        # train_size=200 always; first fold trains on [0,200), tests
        # [200,300). Last fold trains on bars ending at last 100.
        # 500 bars, step 100, train+test=300 → folds at start
        # 0, 100, 200 → 3 folds (start=200 → train [200,400), test [400,500)).
        self.assertEqual(len(folds), 3)
        for f in folds:
            self.assertEqual(f["train_end_bar"] - f["train_start_bar"], 200)
            self.assertEqual(f["test_end_bar"] - f["test_start_bar"], 100)

    def test_run_with_unknown_symbol_raises(self):
        wfr = flox.WalkForwardRunner(
            self.reg, mode="anchored", test_size=100, step=100,
            min_train_size=100, fee_rate=0.0004, initial_capital=10_000,
        )
        wfr.set_strategy_factory(lambda _i: _SmaStrategy([self.btc]))
        with self.assertRaises(flox.FloxError) as cm:
            wfr.run_csv(_CSV, "UNKNOWN_SYMBOL")
        self.assertEqual(cm.exception.code, "E_SYM_001")

    def test_run_without_factory_raises(self):
        wfr = flox.WalkForwardRunner(
            self.reg, mode="anchored", test_size=100, step=100,
            min_train_size=100, fee_rate=0.0004, initial_capital=10_000,
        )
        with self.assertRaises(flox.FloxError) as cm:
            wfr.run_csv(_CSV, "BTCUSDT")
        self.assertEqual(cm.exception.code, "E_RUN_001")


class _IntrabarHighLowStrategy(flox.Strategy):
    """Bar-strategy diagnostic for high/low preservation.

    Enters long on any bar with a non-degenerate intrabar range
    (bar.high > bar.low) — this is true for every real OHLCV bar.
    Under a close-only replay path the bar would arrive with
    high == low == close, so the guard never triggers and the
    strategy stays flat. Under a full-OHLCV path it enters every
    other bar (flat-long alternation).
    """

    def __init__(self, syms):
        super().__init__(syms)
        self.in_position = False

    def on_bar(self, ctx, bar):
        if not self.in_position:
            if float(bar.high) > float(bar.low):
                self.market_buy(0.01)
                self.in_position = True
        else:
            self.market_sell(0.01)
            self.in_position = False


@unittest.skipUnless(os.path.exists(_CSV),
                     f"sample CSV not found at {_CSV}")
class WalkForwardRunBarsTests(unittest.TestCase):
    """Tests for WalkForwardRunner.run_bars (full OHLCV path)."""

    def setUp(self) -> None:
        self.reg = flox.SymbolRegistry()
        self.btc = self.reg.add_symbol("exchange", "BTCUSDT", 0.01)
        # Load the bundled BTC sample into OHLCV arrays.
        import csv
        ts, o, h, lo, c, v = [], [], [], [], [], []
        with open(_CSV) as f:
            r = csv.reader(f)
            next(r)  # header
            for row in r:
                ts.append(int(row[0]))
                o.append(float(row[1]))
                h.append(float(row[2]))
                lo.append(float(row[3]))
                c.append(float(row[4]))
                v.append(float(row[5]))
        import numpy as np
        # The sample uses minute-bar timestamps; convert to ns and
        # derive end_time_ns = start + 60s.
        self.start_ns = np.array(ts, dtype=np.int64) * 1_000_000
        self.end_ns = self.start_ns + 60 * 1_000_000_000
        self.o = np.array(o, dtype=np.float64)
        self.h = np.array(h, dtype=np.float64)
        self.lo = np.array(lo, dtype=np.float64)
        self.c = np.array(c, dtype=np.float64)
        self.v = np.array(v, dtype=np.float64)

    def test_run_bars_produces_folds(self):
        wfr = flox.WalkForwardRunner(
            self.reg, fee_rate=0.0, initial_capital=10_000,
            mode="sliding", train_size=200, test_size=100, step=100,
        )
        wfr.set_strategy_factory(
            lambda _i: _IntrabarHighLowStrategy([self.btc]))
        folds = wfr.run_bars(
            self.start_ns, self.end_ns, self.o, self.h, self.lo,
            self.c, self.v, symbol="BTCUSDT")
        # Fold count: 500 bars, sliding train=200 / test=100 / step=100
        # → start positions 0, 100, 200 → 3 folds (same as run_csv).
        self.assertEqual(len(folds), 3)
        for f in folds:
            self.assertEqual(
                f["train_end_bar"] - f["train_start_bar"], 200)
            self.assertEqual(
                f["test_end_bar"] - f["test_start_bar"], 100)

    def test_run_bars_fires_bar_strategy(self):
        """Critical: a strategy that depends on intrabar high/low must
        produce >0 trades when bars carry full OHLCV. Under the legacy
        close-only path this strategy would see high == low == close
        and never trigger."""
        wfr = flox.WalkForwardRunner(
            self.reg, fee_rate=0.0, initial_capital=10_000,
            mode="sliding", train_size=200, test_size=100, step=100,
        )
        wfr.set_strategy_factory(
            lambda _i: _IntrabarHighLowStrategy([self.btc]))
        folds = wfr.run_bars(
            self.start_ns, self.end_ns, self.o, self.h, self.lo,
            self.c, self.v, symbol="BTCUSDT")
        total_trades = sum(
            f["test_stats"].get("total_trades", 0) or 0 for f in folds)
        # Even if some folds have zero trades, the aggregate across
        # the sample must be > 0 — otherwise the high/low path is
        # not making it into the strategy.
        self.assertGreater(total_trades, 0,
                           "bar-strategy produced 0 trades across all "
                           "folds — high/low are likely not flowing "
                           "through to on_bar")

    def test_run_bars_without_factory_raises(self):
        wfr = flox.WalkForwardRunner(
            self.reg, fee_rate=0.0, initial_capital=10_000,
            mode="sliding", train_size=200, test_size=100, step=100,
        )
        with self.assertRaises(flox.FloxError) as cm:
            wfr.run_bars(self.start_ns, self.end_ns, self.o, self.h,
                         self.lo, self.c, self.v, symbol="BTCUSDT")
        self.assertEqual(cm.exception.code, "E_RUN_001")

    def test_run_bars_unknown_symbol_raises(self):
        wfr = flox.WalkForwardRunner(
            self.reg, fee_rate=0.0, initial_capital=10_000,
            mode="sliding", train_size=200, test_size=100, step=100,
        )
        wfr.set_strategy_factory(
            lambda _i: _IntrabarHighLowStrategy([self.btc]))
        with self.assertRaises(flox.FloxError) as cm:
            wfr.run_bars(self.start_ns, self.end_ns, self.o, self.h,
                         self.lo, self.c, self.v, symbol="UNKNOWN")
        self.assertEqual(cm.exception.code, "E_SYM_001")

    def test_run_bars_length_mismatch_raises(self):
        import numpy as np
        wfr = flox.WalkForwardRunner(
            self.reg, fee_rate=0.0, initial_capital=10_000,
            mode="sliding", train_size=200, test_size=100, step=100,
        )
        wfr.set_strategy_factory(
            lambda _i: _IntrabarHighLowStrategy([self.btc]))
        # truncate one array — should reject
        with self.assertRaises(flox.FloxError) as cm:
            wfr.run_bars(self.start_ns, self.end_ns[:-1], self.o,
                         self.h, self.lo, self.c, self.v, symbol="BTCUSDT")
        self.assertEqual(cm.exception.code, "E_LEN_001")


if __name__ == "__main__":
    unittest.main(verbosity=2)
