"""VenueExecutor.submit_order has to arm its conditional orders.

The simulator decides when a stop or a take-profit fires from
``Order::triggerPrice``. ``python/bare_executor_bindings.h`` filled in
side, price, quantity, type, time-in-force, flags and account and never
touched that field, so every conditional order submitted through a venue
stack carried a zero trigger and fired on the first print it saw -- a
take-profit behaving exactly like the market order the binding would
otherwise have turned it into.

The standalone SimulatedExecutor binding already had the rule these pin:
for the market-style conditionals a single price is unambiguous, so
``price`` is the trigger when the caller leaves ``trigger`` unset, and an
explicit ``trigger`` wins when both matter.
"""
from __future__ import annotations

import flox_py as flox


def _executor():
    # The stack owns the executor, so the caller has to hold on to both.
    stack = flox.VenueStack.binance_um_futures(account_id=1, equity=100_000.0)
    return stack, stack.executor()


def test_a_take_profit_waits_for_its_trigger() -> None:
    _stack, executor = _executor()
    executor.submit_order(1, "sell", 101.3, 1.0, type="tp_market", symbol=1)

    executor.on_trade(1, 101.25, True)
    assert executor.fill_count == 0, "the take-profit fired below its trigger"

    executor.on_trade(1, 101.5, True)
    assert executor.fill_count == 1


def test_a_stop_waits_for_its_trigger() -> None:
    _stack, executor = _executor()
    executor.submit_order(1, "sell", 99.0, 1.0, type="stop_market", symbol=1)

    executor.on_trade(1, 99.5, True)
    assert executor.fill_count == 0, "the stop fired above its trigger"

    executor.on_trade(1, 98.5, True)
    assert executor.fill_count == 1


def test_an_explicit_trigger_is_kept_apart_from_the_limit_price() -> None:
    # A take-profit limit needs both numbers: 95 arms it, 93.5 is the
    # price it then posts.
    _stack, executor = _executor()
    executor.submit_order(1, "sell", 93.5, 1.0, type="tp_limit", trigger=95.0, symbol=1)

    executor.on_trade(1, 94.0, True)
    assert executor.fill_count == 0, "the take-profit limit fired below its trigger"


def test_an_explicit_trigger_wins_over_the_price_on_a_take_profit() -> None:
    # The two numbers only come apart when they disagree, and the
    # market-style conditionals fill straight off the trade feed, so this
    # is where a binding that quietly arms at `price` shows itself: the
    # 1.0 below is a nonsense limit, 101.3 is the level that arms.
    _stack, executor = _executor()
    executor.submit_order(1, "sell", 1.0, 1.0, type="tp_market", trigger=101.3, symbol=1)

    executor.on_trade(1, 50.0, True)
    assert executor.fill_count == 0, "the take-profit armed at the limit price, not the trigger"

    executor.on_trade(1, 101.5, True)
    assert executor.fill_count == 1


def test_an_explicit_trigger_wins_over_the_price_on_a_stop() -> None:
    _stack, executor = _executor()
    executor.submit_order(1, "sell", 1.0, 1.0, type="stop_market", trigger=99.0, symbol=1)

    executor.on_trade(1, 99.5, True)
    assert executor.fill_count == 0, "the stop armed at the limit price, not the trigger"

    executor.on_trade(1, 98.5, True)
    assert executor.fill_count == 1
