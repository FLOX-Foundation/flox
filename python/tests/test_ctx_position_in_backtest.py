"""Pybind11 regression test for the BacktestRunner ↔ Strategy
position-manager wire (mirror of tests/test_backtest_ctx_position.cpp).

The C++ gtest pins the engine-level behavior. This test pins that
the pybind11 binding inherits it through the C ABI — strategies in
Python see `ctx.position` / `ctx.is_long()` / `ctx.is_flat()` track
fills the simulator dispatches.
"""
from __future__ import annotations

import flox_py as flox
import numpy as np


class TrackingStrategy(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.samples = []  # (idx, position, is_long, is_flat)
        self.bar_count = 0

    def on_bar(self, ctx, bar):
        if self.bar_count == 1:
            self.market_buy(0.5)
        self.samples.append(
            (self.bar_count, ctx.position, ctx.is_long(), ctx.is_flat())
        )
        self.bar_count += 1


def _make_bars(n: int):
    start_ns = np.array([i * 60_000_000_000 for i in range(n)], dtype=np.int64)
    end_ns = start_ns + 60_000_000_000
    open_ = np.array([100.0 + i for i in range(n)], dtype=np.float64)
    high = open_ + 0.5
    low = open_ - 0.5
    close = open_ + 0.25
    volume = np.full(n, 100.0)
    return start_ns, end_ns, open_, high, low, close, volume


def test_ctx_position_reflects_fills_after_market_buy() -> None:
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    runner = flox.BacktestRunner(registry, initial_capital=100_000.0)
    strat = TrackingStrategy([sym])
    runner.set_strategy(strat)

    bars = _make_bars(5)
    runner.run_bars(*bars, symbol="BTCUSDT")

    assert len(strat.samples) == 5
    # Bar 0: nothing emitted yet → flat.
    assert strat.samples[0] == (0, 0.0, False, True)
    # Bar 1: ctx is snapshotted before the emit; sample still flat.
    assert strat.samples[1][3] is True, "snapshot taken before market_buy fills"
    # Bars 2–4: refreshPosition picks up the fill from bar 1.
    for i in range(2, 5):
        idx, pos, is_long, is_flat = strat.samples[i]
        assert is_long is True, f"bar {i}: ctx.is_long() should be True"
        assert is_flat is False, f"bar {i}: ctx.is_flat() should be False"
        assert pos == 0.5, f"bar {i}: ctx.position should be 0.5, got {pos}"


def test_flat_guard_prevents_re_entry_after_first_buy() -> None:
    """Without the position-manager wire, ctx.is_flat() is always True
    in backtest, so a flat-guarded entry buys on every bar. This test
    pins that the guard works once a position is open."""

    class GuardedStrategy(flox.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.buys = 0

        def on_bar(self, ctx, bar):
            if ctx.is_flat():
                self.market_buy(0.5)
                self.buys += 1

    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    runner = flox.BacktestRunner(registry, initial_capital=100_000.0)
    strat = GuardedStrategy([sym])
    runner.set_strategy(strat)

    bars = _make_bars(10)
    runner.run_bars(*bars, symbol="BTCUSDT")

    # With the fix: bar 0 flat → buy; from bar 1 onwards ctx is long
    # so the guard prevents re-entry. Exactly one buy.
    # Without the fix: ctx always reports flat → 10 buys.
    assert strat.buys == 1, f"is_flat() guard failed: got {strat.buys} buys"
