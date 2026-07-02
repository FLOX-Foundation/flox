"""Conditional orders through the Python SimulatedExecutor binding.

submit_order accepts stop_market / stop_limit / take_profit_market /
take_profit_limit / trailing_stop, and the trigger must actually reach
Order::triggerPrice (respectively trailingOffset / trailingCallbackRate)
on the C++ side. Before the fix the binding dropped the trigger: a BUY
stop fired on the first trade at any price and a SELL stop never fired.
Each test drives the simulated clock + trade feed directly.
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

import flox_py as flox  # noqa: E402

NS = 1_000_000_000


class _Sim:
    """Tiny driver: seed trades with advancing timestamps."""

    def __init__(self) -> None:
        self.sim = flox.SimulatedExecutor()
        self._t = 0

    def trade(self, price: float, qty: float = 1.0, is_buy: bool = True) -> None:
        self._t += NS
        self.sim.advance_clock(self._t)
        self.sim.on_trade_qty(1, price, qty, is_buy)

    def fills(self):
        return self.sim.fills_list()


class StopMarketTests(unittest.TestCase):
    def test_sell_stop_fires_only_at_or_below_trigger(self) -> None:
        d = _Sim()
        d.sim.submit_order(1, "sell", 95.0, 1.0, type="stop_market", symbol=1)
        d.trade(100.0)
        self.assertEqual(d.fills(), [], "price above trigger must not fire")
        d.trade(94.0)
        fills = d.fills()
        self.assertEqual(len(fills), 1)
        self.assertEqual(fills[0]["price"], 94.0)
        self.assertEqual(fills[0]["side"], "sell")

    def test_buy_stop_fires_only_at_or_above_trigger(self) -> None:
        d = _Sim()
        d.sim.submit_order(2, "buy", 105.0, 1.0, type="stop_market", symbol=1)
        d.trade(100.0)
        self.assertEqual(d.fills(), [], "price below trigger must not fire")
        d.trade(106.0)
        fills = d.fills()
        self.assertEqual(len(fills), 1)
        self.assertEqual(fills[0]["price"], 106.0)

    def test_explicit_trigger_wins_over_price(self) -> None:
        d = _Sim()
        # price param carries a (nonsense) limit value; trigger is explicit
        d.sim.submit_order(3, "sell", 1.0, 1.0, type="stop_market",
                           trigger=95.0, symbol=1)
        d.trade(96.0)
        self.assertEqual(d.fills(), [])
        d.trade(95.0)
        self.assertEqual(len(d.fills()), 1)


class StopLimitTests(unittest.TestCase):
    def test_trigger_converts_to_resting_limit(self) -> None:
        d = _Sim()
        d.sim.submit_order(4, "sell", 93.5, 1.0, type="stop_limit",
                           trigger=95.0, symbol=1)
        d.trade(100.0)
        self.assertEqual(d.fills(), [])
        d.trade(94.0)  # trigger fires (94 <= 95); limit 93.5 goes live
        self.assertEqual(d.fills(), [], "resting limit needs book liquidity")
        # Resting limits fill against the book, not the trade feed:
        # provide a bid at 94 so the SELL limit at 93.5 crosses.
        d.sim.on_book_snapshot(1, [(94.0, 5.0)], [(94.5, 5.0)])
        d.trade(94.0)
        fills = d.fills()
        self.assertEqual(len(fills), 1, fills)
        self.assertGreaterEqual(fills[0]["price"], 93.5)


class TakeProfitTests(unittest.TestCase):
    def test_sell_tp_fires_at_or_above_trigger(self) -> None:
        d = _Sim()
        d.sim.submit_order(5, "sell", 101.3, 1.0,
                           type="take_profit_market", symbol=1)
        d.trade(101.25)
        self.assertEqual(d.fills(), [], "below trigger must not fire")
        d.trade(101.5)
        fills = d.fills()
        self.assertEqual(len(fills), 1)
        self.assertEqual(fills[0]["price"], 101.5)


class TrailingStopTests(unittest.TestCase):
    def test_fixed_offset_ratchets_and_fires_on_pullback(self) -> None:
        d = _Sim()
        d.trade(100.0)  # seed market so activation price is known
        d.sim.submit_order(6, "sell", 0.0, 1.0, type="trailing_stop",
                           trailing_offset=0.3, symbol=1)
        for px in (100.5, 101.0, 101.5):
            d.trade(px)
        self.assertEqual(d.fills(), [], "no fire while ratcheting up")
        d.trade(101.1)  # 101.1 <= 101.5 - 0.3
        fills = d.fills()
        self.assertEqual(len(fills), 1)
        self.assertEqual(fills[0]["price"], 101.1)

    def test_bps_offset_variant(self) -> None:
        d = _Sim()
        d.trade(100.0)
        # 100 bps = 1%: trigger trails 1% under the high-water mark
        d.sim.submit_order(7, "sell", 0.0, 1.0, type="trailing_stop",
                           trailing_bps=100, symbol=1)
        d.trade(102.0)   # trigger ratchets to ~100.98
        self.assertEqual(d.fills(), [])
        d.trade(100.9)   # below the trailed trigger
        self.assertEqual(len(d.fills()), 1)


if __name__ == "__main__":
    unittest.main()
