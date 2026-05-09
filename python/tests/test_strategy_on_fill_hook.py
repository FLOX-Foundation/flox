"""Tests for the new Strategy.on_fill / on_order_update hooks.

Without these hooks the strategy never learns that an order it
emitted has filled. Native stops are unusable (no callback to
reset local state on a stop fire) and exit logic that wants to
confirm "the close went through" has nothing to hang on.
"""
from __future__ import annotations

import flox_py as flox
import numpy as np


def _make_bars(n: int):
    start_ns = np.array([i * 60_000_000_000 for i in range(n)], dtype=np.int64)
    end_ns = start_ns + 60_000_000_000
    open_ = np.array([100.0 + i for i in range(n)], dtype=np.float64)
    high = open_ + 0.5
    low = open_ - 0.5
    close = open_ + 0.25
    volume = np.full(n, 100.0)
    return start_ns, end_ns, open_, high, low, close, volume


class FillRecorder(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fills = []
        self.updates = []
        self.bar_count = 0

    def on_bar(self, ctx, bar):
        if self.bar_count == 1:
            self.market_buy(0.5)
        self.bar_count += 1

    def on_fill(self, ctx, ev):
        self.fills.append({
            "order_id": ev.order_id,
            "symbol_id": ev.symbol_id,
            "side": ev.side,
            "order_type": ev.order_type,
            "status": ev.status,
            "fill_qty": ev.fill_qty,
            "fill_price": ev.fill_price,
        })

    def on_order_update(self, ctx, ev):
        self.updates.append({
            "status": ev.status,
            "order_id": ev.order_id,
        })


def test_on_fill_fires_after_market_buy() -> None:
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    runner = flox.BacktestRunner(registry, fee_rate=0.0004,
                                  initial_capital=100_000.0)
    strat = FillRecorder([sym])
    runner.set_strategy(strat)

    bars = _make_bars(5)
    runner.run_bars(*bars, symbol="BTCUSDT")

    # market_buy(0.5) on bar 1 → executor fills synchronously → on_fill
    # gets called with FILLED status.
    assert len(strat.fills) >= 1, "on_fill should fire for the market_buy"
    fill = strat.fills[0]
    assert fill["side"] == "buy"
    assert fill["order_type"] == "market"
    assert fill["status"] == "FILLED"
    assert fill["fill_qty"] == 0.5
    assert fill["fill_price"] > 0


def test_on_order_update_fires_for_non_fill_events() -> None:
    """Submitted/Accepted/Rejected/etc go to on_order_update, not on_fill."""
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    runner = flox.BacktestRunner(registry, fee_rate=0.0004,
                                  initial_capital=100_000.0)
    strat = FillRecorder([sym])
    runner.set_strategy(strat)

    bars = _make_bars(5)
    runner.run_bars(*bars, symbol="BTCUSDT")

    # The simulator emits SUBMITTED + ACCEPTED for every order before
    # it fills. Those go through on_order_update.
    statuses = {u["status"] for u in strat.updates}
    assert "SUBMITTED" in statuses or "ACCEPTED" in statuses, \
        f"expected non-fill updates, got: {statuses}"
