"""Tests for the composite-condition DSL.

Uses a minimal stub strategy that just exposes `last_n_closed_bars`
the way the real C++ Strategy does, so we can exercise the DSL without
booting an engine.
"""
from __future__ import annotations

import math
from typing import List

import pytest

from flox_py.composite import TIME_BARS, when


class StubStrategy:
    """Implements just enough of the Strategy surface for the DSL.

    Real strategies use the C++ ring populated by the engine; this
    stub stores bars in memory and returns them on demand.
    """

    def __init__(self) -> None:
        self._bars: dict[tuple[int, int, int], List[dict]] = {}

    def push(self, symbol_id: int, bar_type: int, param: int, close: float) -> None:
        key = (symbol_id, bar_type, param)
        self._bars.setdefault(key, []).append({
            "open": close, "high": close, "low": close, "close": close,
            "volume": 0.0, "start_ns": 0, "end_ns": 0,
        })

    def last_n_closed_bars(self, sym: int, bt: int, p: int, n: int) -> List[dict]:
        ring = self._bars.get((sym, bt, p), [])
        return ring[-n:]


H1 = 3_600_000_000_000
M5 = 300_000_000_000


def test_compare_warmup_then_fires() -> None:
    s = StubStrategy()
    cond = when(s, 1, TIME_BARS, H1).sma(3) > 100.0
    assert not cond.is_ready()  # no bars yet
    for c in (101, 102, 103):
        s.push(1, TIME_BARS, H1, c)
    assert cond.is_ready()
    assert cond.value() is True  # avg(101,102,103)=102 > 100


def test_and_or_not() -> None:
    s = StubStrategy()
    for c in (50, 60, 70, 80, 90):
        s.push(1, TIME_BARS, H1, c)
    cond_up = when(s, 1, TIME_BARS, H1).sma(3) > 60.0
    cond_low = when(s, 1, TIME_BARS, H1).close() < 100.0
    assert (cond_up & cond_low).value() is True
    assert (cond_up | cond_low).value() is True
    assert (~cond_low).value() is False


def test_multi_tf_separate_rings() -> None:
    s = StubStrategy()
    s.push(1, TIME_BARS, H1, 10.0)
    s.push(1, TIME_BARS, H1, 20.0)
    s.push(1, TIME_BARS, M5, 5.0)
    s.push(1, TIME_BARS, M5, 5.0)
    cond = (
        when(s, 1, TIME_BARS, H1).sma(2)
        > when(s, 1, TIME_BARS, M5).sma(2)
    )
    assert cond.is_ready()
    assert cond.value() is True  # H1 avg=15 > M5 avg=5


def test_rsi_warmup() -> None:
    s = StubStrategy()
    cond = when(s, 1, TIME_BARS, H1).rsi(3) < 50.0
    # Need 4 bars (3+1) before RSI is ready
    for c in (100, 99, 98):
        s.push(1, TIME_BARS, H1, c)
    assert not cond.is_ready()
    s.push(1, TIME_BARS, H1, 97)
    assert cond.is_ready()
    # All-decline => RSI = 0 < 50 => True
    assert cond.value() is True


def test_const_compare() -> None:
    s = StubStrategy()
    s.push(1, TIME_BARS, H1, 100.0)
    s.push(1, TIME_BARS, H1, 110.0)
    cond_eq = when(s, 1, TIME_BARS, H1).close() == 110.0
    assert cond_eq.value() is True
    cond_le = when(s, 1, TIME_BARS, H1).close() <= 200.0
    assert cond_le.value() is True


def test_not_ready_when_one_side_warmup() -> None:
    s = StubStrategy()
    s.push(1, TIME_BARS, H1, 10.0)  # only 1 bar
    cond = when(s, 1, TIME_BARS, H1).sma(5) > 0.0
    assert not cond.is_ready()
    # Calling .value() on a not-ready node returns nan, so the bool is
    # well-defined but the strategy must guard with .is_ready().
    assert math.isnan(cond._l.value())  # type: ignore[attr-defined]
