"""The Python binding's order-type names must be the canonical ones.

``include/flox/capi/order_type_names.hpp`` is the one string <-> code table
for the flox::OrderType space, and its header says every binding "must go
through this header instead of keeping its own private copy". The Python
binding kept three private copies instead
(``python/strategy_bindings.h:orderTypeName``,
``python/backtest_bindings.h:parseOrderType``,
``python/bare_executor_bindings.h:parseOrderType``), and they drifted:

  * they spell code 4/5 ``take_profit_market`` / ``take_profit_limit``
    where the canonical table says ``tp_market`` / ``tp_limit``;
  * ``orderTypeName`` has no case 7, so ICEBERG reads back ``unknown``;
  * both ``parseOrderType`` copies fall through to ``OrderType::MARKET``
    for any name they do not recognise -- so handing them the canonical
    ``tp_market`` silently submits a *market* order, and so does a typo.

These pin the contract: names come from the canonical table, every code
round-trips through its name, and an unrecognised name raises instead of
turning into a market order.
"""
from __future__ import annotations

import pytest

import flox_py as flox

NS = 1_000_000_000

# Space A (flox::OrderType) as order_type_names.hpp spells it, in code order.
CANONICAL_NAMES = (
    "limit",
    "market",
    "stop_market",
    "stop_limit",
    "tp_market",
    "tp_limit",
    "trailing_stop",
    "iceberg",
)


def _sim():
    return flox.SimulatedExecutor()


def _trade(sim, t_ns: int, price: float) -> None:
    sim.advance_clock(t_ns)
    sim.on_trade_qty(1, price, 1.0, True)


# ── The canonical table, exposed to Python ────────────────────────────


def test_order_type_names_are_the_canonical_table() -> None:
    assert tuple(flox.ORDER_TYPE_NAMES) == CANONICAL_NAMES


def test_every_code_round_trips_through_its_name() -> None:
    for code, name in enumerate(CANONICAL_NAMES):
        assert flox.order_type_name(code) == name
        assert flox.order_type_code(name) == code


def test_iceberg_is_code_seven_not_unknown() -> None:
    assert flox.order_type_name(7) == "iceberg"
    assert flox.order_type_code("iceberg") == 7


def test_a_code_outside_the_table_raises() -> None:
    with pytest.raises(ValueError):
        flox.order_type_name(len(CANONICAL_NAMES))


def test_an_unknown_name_raises_and_names_the_accepted_set() -> None:
    with pytest.raises(ValueError) as excinfo:
        flox.order_type_code("banana")
    message = str(excinfo.value)
    assert "banana" in message
    for name in CANONICAL_NAMES:
        assert name in message, f"{name!r} missing from {message!r}"


# ── SimulatedExecutor.submit_order (backtest_bindings.h) ──────────────


def test_tp_market_submits_a_take_profit_not_a_market_order() -> None:
    # A sell take-profit at 101.3 must wait for the market to reach the
    # trigger and then book its own 101.3. A market order would have
    # filled on the first print at 101.25.
    sim = _sim()
    sim.submit_order(1, "sell", 101.3, 1.0, type="tp_market", symbol=1)
    _trade(sim, NS, 101.25)
    assert sim.fills_list() == [], "tp_market filled like a market order"
    _trade(sim, 2 * NS, 101.5)
    fills = sim.fills_list()
    assert len(fills) == 1
    assert fills[0]["price"] == 101.3


def test_tp_limit_is_accepted_under_its_canonical_name() -> None:
    sim = _sim()
    sim.submit_order(2, "sell", 93.5, 1.0, type="tp_limit", trigger=95.0, symbol=1)
    _trade(sim, NS, 100.0)
    assert sim.fills_list() == [], "tp_limit filled like a market order"


def test_the_legacy_long_names_are_no_longer_accepted() -> None:
    # "take_profit_market" is not in the canonical table. Accepting it
    # keeps the drift alive in every strategy that copies it from a doc.
    for legacy in ("take_profit_market", "take_profit_limit"):
        sim = _sim()
        with pytest.raises(ValueError) as excinfo:
            sim.submit_order(3, "sell", 101.3, 1.0, type=legacy, symbol=1)
        assert legacy in str(excinfo.value)
        _trade(sim, NS, 101.25)
        assert sim.fill_count == 0, "a refused order still reached the book"


def test_an_unknown_order_type_raises_instead_of_going_to_market() -> None:
    sim = _sim()
    with pytest.raises(ValueError) as excinfo:
        sim.submit_order(4, "buy", 100.0, 1.0, type="markte", symbol=1)
    message = str(excinfo.value)
    assert "markte" in message
    for name in CANONICAL_NAMES:
        assert name in message, f"{name!r} missing from {message!r}"

    _trade(sim, NS, 100.0)
    assert sim.fill_count == 0, "a typo submitted a market order"


def test_bracket_legs_reject_an_unknown_order_type_too() -> None:
    sim = _sim()
    with pytest.raises(ValueError):
        sim.submit_bracket(
            1,
            1,
            "buy",
            "market",
            100.0,
            1.0,
            "sell",
            "take_profit_market",
            110.0,
            "sell",
            "stop_market",
            90.0,
        )


# ── VenueExecutor.submit_order (bare_executor_bindings.h) ─────────────


def _venue_executor():
    # The stack owns the executor, so the caller has to hold on to both.
    stack = flox.VenueStack.binance_um_futures(account_id=1, equity=100_000.0)
    return stack, stack.executor()


def test_venue_executor_accepts_the_canonical_names() -> None:
    _stack, executor = _venue_executor()
    for name in CANONICAL_NAMES:
        executor.submit_order(
            flox.order_type_code(name) + 1, "buy", 100.0, 1.0, type=name, symbol=1
        )


def test_venue_executor_refuses_the_legacy_and_unknown_names() -> None:
    _stack, executor = _venue_executor()
    for bad in ("take_profit_market", "take_profit_limit", "markte"):
        with pytest.raises(ValueError) as excinfo:
            executor.submit_order(1, "buy", 100.0, 1.0, type=bad, symbol=1)
        assert bad in str(excinfo.value)
    executor.on_bar(1, 100.0)
    assert executor.fill_count == 0, "a refused order still reached the book"


# ── Order events (strategy_bindings.h) ────────────────────────────────


class _TakeProfitStrategy(flox.Strategy):
    """Buys on the first bar, then hangs a take-profit above the market."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.bar_count = 0
        self.fills = []

    def on_bar(self, ctx, bar):
        if self.bar_count == 0:
            self.market_buy(1.0)
        elif self.bar_count == 1:
            self.take_profit_market(side="sell", trigger=102.0, qty=1.0)
        self.bar_count += 1

    def on_fill(self, ctx, ev):
        self.fills.append({"side": ev.side, "order_type": ev.order_type})


def _bars(n: int):
    import numpy as np

    start_ns = np.array([i * 60 * NS for i in range(n)], dtype=np.int64)
    end_ns = start_ns + 60 * NS
    open_ = np.array([100.0 + i for i in range(n)], dtype=np.float64)
    high = open_ + 0.5
    low = open_ - 0.5
    close = open_ + 0.25
    volume = np.full(n, 100.0)
    return start_ns, end_ns, open_, high, low, close, volume


def test_a_take_profit_fill_event_carries_the_canonical_name() -> None:
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    runner = flox.BacktestRunner(registry, fee_rate=0.0, initial_capital=100_000.0)
    strat = _TakeProfitStrategy([sym])
    runner.set_strategy(strat)
    runner.run_bars(*_bars(6), symbol="BTCUSDT")

    sells = [f for f in strat.fills if f["side"] == "sell"]
    assert sells, f"the take-profit never filled: {strat.fills}"
    assert sells[0]["order_type"] == "tp_market"
