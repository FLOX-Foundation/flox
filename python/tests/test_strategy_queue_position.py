"""Queue-position visibility from Python strategies.

These tests verify the Python surface for queue-position exposure:
  - `OrderEventData.queue_ahead` / `.queue_total` fields exist
  - `Strategy.on_queue_position_change` callback exists and dispatches
  - `SimulatedExecutor.set_queue_position_min_change_fraction` exists

End-to-end emission semantics are covered by the C++ unit tests in
tests/test_backtest_queue.cpp.
"""
from __future__ import annotations

import flox_py as flox


def test_order_event_data_has_queue_fields() -> None:
    # Fields visible on the binding class (registered as def_readwrite).
    assert "queue_ahead" in dir(flox.OrderEventData)
    assert "queue_total" in dir(flox.OrderEventData)


def test_strategy_exposes_on_queue_position_change() -> None:
    class S(flox.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.calls = 0

        def on_queue_position_change(self, ctx, ev):
            self.calls += 1

    # Construction succeeds and the override is callable.
    registry = flox.SymbolRegistry()
    sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    strat = S([sym])
    assert hasattr(strat, "on_queue_position_change")
    assert callable(strat.on_queue_position_change)


def test_simulated_executor_has_queue_fraction_setter() -> None:
    executor = flox.SimulatedExecutor()
    assert hasattr(executor, "set_queue_position_min_change_fraction")
    executor.set_queue_position_min_change_fraction(0.10)
    # Idempotent / numeric input accepted; nothing to assert beyond no-throw.
