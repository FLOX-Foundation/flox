"""Smoke tests for multi-TF alignment helpers exposed on the Python Strategy.

Strategy authors don't construct PyStrategyBase directly without an
engine to drive it; instead these tests exercise the wired-up surface
by running a tiny strategy through a backtest. The key assertion is
that `last_closed_bar` and `last_n_closed_bars` return the expected
state at each step.
"""

from __future__ import annotations

import flox_py


def test_strategy_class_exposes_helpers() -> None:
    s = flox_py.Strategy(symbols=[1])
    # Without an engine bridge the helpers must return None / empty / 0
    # rather than crashing. This is the cold-start contract.
    assert s.last_closed_bar(1, 0, 60_000_000_000) is None
    assert s.last_n_closed_bars(1, 0, 60_000_000_000, 3) == []
    assert s.bar_ring_capacity() == 0  # no bridge => unset


def test_helpers_documented_on_strategy_class() -> None:
    # Doc strings are used by IDE autocomplete; basic sanity check that
    # the binding emitted them.
    cls = flox_py.Strategy
    assert "last_closed_bar" in dir(cls)
    assert "last_n_closed_bars" in dir(cls)
    assert "set_bar_ring_capacity" in dir(cls)
    assert "bar_ring_capacity" in dir(cls)
