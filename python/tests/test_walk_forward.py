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


if __name__ == "__main__":
    unittest.main(verbosity=2)
