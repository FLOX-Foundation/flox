"""`flox engine sim` type joins.

Every one of these asserts a boundary where two flox objects that look
interchangeable are not. The command shipped in #234 raised TypeError on the
first of them, so it had never started once; each fix below just moved the
failure to the next line. These tests pin the joins so the command cannot
regress back into never-running without a test failing.
"""
import pytest

import flox_py
from flox_py import engine_cli


def _runner() -> flox_py.Runner:
    reg = flox_py.SymbolRegistry()
    reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    return flox_py.Runner(reg, on_signal=lambda s: None, threaded=False)


def test_simulated_executor_is_not_a_runner_executor() -> None:
    # The premise of _make_sim_bridge. If this ever starts passing, the
    # bridge is redundant and should go.
    with pytest.raises(TypeError):
        _runner().set_executor(flox_py.SimulatedExecutor())


def test_sim_bridge_is_accepted_by_the_runner() -> None:
    _runner().set_executor(engine_cli._make_sim_bridge(flox_py.SimulatedExecutor()))


def test_sim_bridge_forwards_orders_into_the_simulator() -> None:
    sim = flox_py.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)

    order = flox_py.Order()
    order.id = 7
    order.side = "buy"
    order.price = 100.0
    order.quantity = 1.0
    order.order_type = "limit"
    order.symbol = 1
    order.time_in_force = "gtc"

    bridge.submit(order)
    bridge.cancel(7)
    bridge.cancel_all(1)


def test_kill_switch_stub_is_not_a_runner_kill_switch() -> None:
    with pytest.raises(TypeError):
        _runner().set_kill_switch(engine_cli._KillSwitch())


def test_kill_switch_bridge_reflects_the_stub_state() -> None:
    state = engine_cli._KillSwitch()
    bridge = engine_cli._make_kill_switch_bridge(state)
    _runner().set_kill_switch(bridge)

    sig = flox_py.Signal()
    # True lets the signal through; the switch starts open.
    assert bridge.check(sig) is True

    # Flipping it over the control surface has to halt the engine too --
    # before the bridge existed it gated only the HTTP endpoints while the
    # strategy kept trading.
    state.set(True, reason="test")
    assert bridge.check(sig) is False

    state.set(False)
    assert bridge.check(sig) is True


def test_add_symbol_returns_a_symbol_not_an_int() -> None:
    # Why cmd_engine_sim takes .id: the control server and the state writer
    # put the symbol id straight into JSON, and Symbol is not serialisable.
    import json

    reg = flox_py.SymbolRegistry()
    symbol = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)

    with pytest.raises(TypeError):
        json.dumps({"symbols": [symbol]})

    assert json.loads(json.dumps({"symbols": [int(symbol.id)]}))["symbols"] == [1]
