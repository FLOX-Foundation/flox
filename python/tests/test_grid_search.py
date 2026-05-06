"""Tests for ``flox_py.GridSearch``.

Exercises axis registration, params decoding, factory contract, and a
realistic 2-axis sweep on the bundled BTC sample.
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


class GridSearchTests(unittest.TestCase):

    def test_total_zero_with_no_axes(self):
        gs = flox.GridSearch()
        self.assertEqual(gs.total(), 0)

    def test_axis_product_is_total(self):
        gs = flox.GridSearch()
        gs.add_axis([1.0, 2.0, 3.0])
        gs.add_axis([10.0, 20.0])
        self.assertEqual(gs.total(), 6)

    def test_params_for_index_decodes_row_major(self):
        gs = flox.GridSearch()
        gs.add_axis([1.0, 2.0])
        gs.add_axis([10.0, 20.0, 30.0])
        # Last axis varies fastest — index 0..5 cover all 6 points.
        expected = [
            [1.0, 10.0], [1.0, 20.0], [1.0, 30.0],
            [2.0, 10.0], [2.0, 20.0], [2.0, 30.0],
        ]
        got = [list(gs.params_for_index(i)) for i in range(6)]
        self.assertEqual(got, expected)

    def test_run_calls_factory_with_each_combination(self):
        gs = flox.GridSearch()
        gs.add_axis([1.0, 2.0])
        gs.add_axis([10.0, 20.0])
        seen = []

        def factory(params):
            seen.append(tuple(params))
            return {"sharpe": float(params[0] + params[1]),
                    "return_pct": 0.0, "total_trades": 0}

        gs.set_factory(factory)
        results = gs.run()
        self.assertEqual(len(results), 4)
        self.assertEqual(len(seen), 4)
        # Stats round-trip through the result dict.
        self.assertAlmostEqual(results[0]["stats"]["sharpe"], 11.0)
        self.assertAlmostEqual(results[3]["stats"]["sharpe"], 22.0)

    def test_run_without_factory_raises(self):
        gs = flox.GridSearch()
        gs.add_axis([1.0])
        with self.assertRaises(flox.FloxError) as cm:
            gs.run()
        self.assertEqual(cm.exception.code, "E_RUN_001")

    def test_run_with_no_axes_raises(self):
        gs = flox.GridSearch()
        gs.set_factory(lambda p: {"sharpe": 0})
        with self.assertRaises(flox.FloxError) as cm:
            gs.run()
        self.assertEqual(cm.exception.code, "E_RUN_002")

    @unittest.skipUnless(os.path.exists(_CSV),
                         f"sample CSV not found at {_CSV}")
    def test_realistic_sweep_returns_distinct_sharpe(self):
        reg = flox.SymbolRegistry()
        btc = reg.add_symbol("exchange", "BTCUSDT", 0.01)

        def factory(params):
            fast, slow = int(params[0]), int(params[1])
            if fast >= slow:
                return {"sharpe": 0.0, "return_pct": 0.0, "total_trades": 0}

            class _S(flox.Strategy):
                def __init__(self, syms):
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
                    elif f < s and ctx.is_flat():
                        self.market_sell(0.01)

            bt = flox.BacktestRunner(reg, 0.0004, 10_000)
            bt.set_strategy(_S([btc]))
            return bt.run_csv(_CSV, symbol="BTCUSDT")

        gs = flox.GridSearch()
        gs.add_axis([5.0, 10.0])
        gs.add_axis([20.0, 30.0])
        gs.set_factory(factory)
        results = gs.run()
        self.assertEqual(len(results), 4)
        sharpes = [r["stats"]["sharpe"] for r in results]
        self.assertGreater(len(set(sharpes)), 1,
                           "different params should yield different sharpe")


if __name__ == "__main__":
    unittest.main(verbosity=2)
