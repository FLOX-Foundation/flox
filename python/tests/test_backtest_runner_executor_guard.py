"""BacktestRunner.set_executor() must refuse a VenueStack executor.

BacktestRunner routes signals to a custom executor but keeps feeding
market data only to its built-in simulator, and the contract puts fill
reporting on the custom executor. A VenueStack executor therefore
receives orders but no data: the run completes with zero trades and a
flat equity curve, which reads like a broken strategy rather than a
wiring mistake. The binding refuses it with an explanation instead.
"""
from __future__ import annotations

import pytest

import flox_py as flox


def _runner() -> flox.BacktestRunner:
    reg = flox.SymbolRegistry()
    reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    return flox.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)


def test_venue_stack_executor_is_refused_with_guidance() -> None:
    stack = flox.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)

    with pytest.raises(ValueError) as excinfo:
        _runner().set_executor(stack.executor())

    msg = str(excinfo.value)
    # The message has to name the supported pattern, not just say "no".
    assert "VenueExecutor" in msg
    assert "ingest_executor" in msg


def test_custom_executor_overload_still_accepted() -> None:
    class NullExecutor(flox.Executor):
        def submit_order(self, order) -> None:  # noqa: ANN001
            pass

    bt = _runner()
    bt.set_executor(NullExecutor())
    bt.set_executor(None)
