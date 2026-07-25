"""VenueStack.clock() and the SimulatedClock it returns.

The method was bound without its return type ever being registered with
pybind, so every call raised "Unregistered type : flox::SimulatedClock" --
a method that could not succeed under any argument. Driving the clock is the
point of holding a stack from Python (RL envs, paper harnesses), where
nothing else advances simulated time.
"""
import flox_py


def _stack() -> "flox_py.VenueStack":
    return flox_py.VenueStack.binance_um_futures(account_id=1, equity=1_000.0)


def test_clock_is_reachable() -> None:
    assert _stack().clock().now_ns() == 0


def test_advance_to_moves_time_forward() -> None:
    clock = _stack().clock()
    clock.advance_to(5_000)
    assert clock.now_ns() == 5_000


def test_advance_to_is_monotonic() -> None:
    clock = _stack().clock()
    clock.advance_to(5_000)
    clock.advance_to(3_000)
    assert clock.now_ns() == 5_000


def test_reset_moves_time_backwards() -> None:
    clock = _stack().clock()
    clock.advance_to(5_000)
    clock.reset(100)
    assert clock.now_ns() == 100
    clock.reset()
    assert clock.now_ns() == 0


def test_clock_is_the_one_the_stack_runs_on() -> None:
    # reference_internal, so repeated calls hand back the same clock rather
    # than a copy that silently diverges from the executor's view of time.
    stack = _stack()
    stack.clock().advance_to(7_000)
    assert stack.clock().now_ns() == 7_000
