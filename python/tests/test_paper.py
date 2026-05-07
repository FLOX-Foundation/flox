"""Tests for ``flox_py.paper.PaperBroker``."""
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

from flox_py import paper  # noqa: E402


def _build_broker() -> tuple[paper.PaperBroker, int]:
    """Common scaffolding: a registered symbol and a fresh broker."""
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("paper", "BTCUSDT", tick_size=0.01)
    broker = paper.PaperBroker(registry=registry)
    return broker, int(sym)


class _MarketOrderStrategy(flox.Strategy):
    """Sends one market BUY when the first trade comes in."""

    def __init__(self, symbols, qty: float = 1.0):
        super().__init__(symbols)
        self.qty = qty
        self._fired = False

    def on_trade(self, ctx, trade):
        if self._fired:
            return
        self._fired = True
        self.market_buy(self.qty)


class PaperBrokerTests(unittest.TestCase):
    def test_market_buy_simulates_a_fill(self) -> None:
        broker, sym = _build_broker()
        strat = _MarketOrderStrategy([sym], qty=1.5)
        broker.runner.add_strategy(strat)

        broker.start()
        # First trade triggers the strategy's market_buy.
        broker.observe_trade(sym, 100.0, 1.0, True, ts_ns=1_000_000_000)
        # Subsequent trades let the simulator settle the fill.
        broker.observe_trade(sym, 101.0, 2.0, False, ts_ns=2_000_000_000)
        broker.stop()

        fills = broker.fills_list()
        self.assertEqual(len(fills), 1, f"expected 1 fill, got {fills}")
        f = fills[0]
        self.assertEqual(f["symbol"], sym)
        self.assertGreater(f["price"], 0.0)
        self.assertGreater(f["quantity"], 0.0)

        self.assertGreaterEqual(broker.stats.orders_simulated, 1)
        self.assertGreaterEqual(broker.stats.trades_observed, 2)
        self.assertIsNone(broker.stats.error)

    def test_user_on_signal_callback_fires_after_routing(self) -> None:
        registry = flox.SymbolRegistry()
        sym = int(registry.add_symbol("paper", "BTCUSDT", tick_size=0.01))
        seen = []

        broker = paper.PaperBroker(
            registry=registry,
            on_signal=lambda sig: seen.append(sig),
        )

        strat = _MarketOrderStrategy([sym], qty=0.5)
        broker.runner.add_strategy(strat)
        broker.start()
        broker.observe_trade(sym, 50.0, 1.0, True, ts_ns=1_000_000_000)
        broker.stop()

        self.assertEqual(len(seen), 1)
        # Routing must happen before the callback so the simulator
        # already accepted the order by the time the user sees it.
        self.assertGreaterEqual(broker.stats.orders_simulated, 1)

    def test_slippage_fixed_bps_applied(self) -> None:
        registry = flox.SymbolRegistry()
        sym = int(registry.add_symbol("paper", "BTCUSDT", tick_size=0.01))
        broker = paper.PaperBroker(
            registry=registry,
            default_slippage_model="fixed_bps",
            default_slippage_params={"bps": 10.0},  # 10 bps
        )

        strat = _MarketOrderStrategy([sym], qty=1.0)
        broker.runner.add_strategy(strat)
        broker.start()
        # First trade at 100.0 triggers the buy.
        broker.observe_trade(sym, 100.0, 1.0, True, ts_ns=1_000_000_000)
        broker.observe_trade(sym, 100.0, 1.0, False, ts_ns=2_000_000_000)
        broker.stop()

        fills = broker.fills_list()
        self.assertEqual(len(fills), 1)
        # 10 bps = 0.10% = 0.10 on a 100.0 price. Fill price should be
        # higher than 100.0 because slippage works against the taker
        # (a buy gets a worse — higher — price).
        fill_price = fills[0]["price"]
        self.assertGreater(
            fill_price, 100.0,
            f"buy fill should be slipped above 100.0, got {fill_price}",
        )

    def test_unknown_slippage_model_raises(self) -> None:
        registry = flox.SymbolRegistry()
        registry.add_symbol("paper", "BTCUSDT", tick_size=0.01)
        with self.assertRaises(ValueError):
            paper.PaperBroker(
                registry=registry,
                default_slippage_model="bogus",
            )


if __name__ == "__main__":
    unittest.main()
