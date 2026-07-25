"""BacktestRunner driven by a VenueStack.

set_executor() routes order submission to a custom executor but keeps
feeding market data to the built-in simulator, so a venue stack attached
that way produced a run with zero trades. set_venue_stack() wires both
directions: the runner feeds the stack's executor and harvests its fills.
"""
from __future__ import annotations

import pathlib
from collections import deque

import pytest

import flox_py as flox

# Resolve against the repo, not the caller's cwd: CI runs these files
# from several working directories.
CSV = str(
    pathlib.Path(__file__).resolve().parents[1]
    / "flox_py/templates/research/data/btcusdt_sample.csv"
)


class CrossAboveSMA(flox.Strategy):
    def __init__(self, symbols, period: int = 20) -> None:
        super().__init__(symbols)
        self._w: deque[float] = deque(maxlen=period)

    def on_trade(self, ctx, trade) -> None:  # noqa: ANN001
        self._w.append(trade.price)
        if len(self._w) < self._w.maxlen:
            return
        sma = sum(self._w) / len(self._w)
        if trade.price > sma and ctx.is_flat():
            self.market_buy(1.0)
        elif trade.price < sma and ctx.is_long():
            self.close_position()


def _runner_with(stack) -> tuple[flox.BacktestRunner, object]:  # noqa: ANN001
    reg = flox.SymbolRegistry()
    btc = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    bt = flox.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(CrossAboveSMA([btc]))
    if stack is not None:
        bt.set_venue_stack(stack)
    return bt, btc


def test_venue_stack_run_produces_trades_and_venue_fills() -> None:
    stack = flox.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
    bt, _ = _runner_with(stack)

    stats = bt.run_csv(CSV, "BTCUSDT")

    assert stats["total_trades"] > 0, "a venue-stack run must not be silently empty"
    assert len(stack.executor().fills_list()) > 0, (
        "the venue executor must receive market data and fill orders"
    )


def test_venue_stack_matches_the_bare_runner_on_trade_count() -> None:
    bare, _ = _runner_with(None)
    bare_stats = bare.run_csv(CSV, "BTCUSDT")

    stack = flox.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
    venue, _ = _runner_with(stack)
    venue_stats = venue.run_csv(CSV, "BTCUSDT")

    # Same strategy and data; the venue path changes fill mechanics, not
    # the signal stream, so the trade count has to line up.
    assert venue_stats["total_trades"] == bare_stats["total_trades"]


def test_detaching_reverts_to_the_built_in_executor() -> None:
    stack = flox.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
    bt, _ = _runner_with(stack)
    bt.set_venue_stack(None)

    stats = bt.run_csv(CSV, "BTCUSDT")

    assert stats["total_trades"] > 0
    assert len(stack.executor().fills_list()) == 0, (
        "a detached venue executor must not see the run"
    )


def test_bare_venue_executor_is_refused_with_guidance() -> None:
    stack = flox.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
    bt, _ = _runner_with(None)

    with pytest.raises(ValueError) as excinfo:
        bt.set_executor(stack.executor())

    assert "set_venue_stack" in str(excinfo.value)
