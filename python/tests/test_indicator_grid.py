"""Tests for the indicator-grid sugar."""
from __future__ import annotations

from typing import List

from flox_py.composite import TIME_BARS, grid


class StubStrategy:
    """Same minimal stub as the composite-conditions tests."""

    def __init__(self) -> None:
        self._bars: dict[tuple[int, int, int], List[dict]] = {}

    def push(self, sym: int, bt: int, p: int, close: float) -> None:
        self._bars.setdefault((sym, bt, p), []).append({
            "open": close, "high": close, "low": close, "close": close,
            "volume": 0.0, "start_ns": 0, "end_ns": 0,
        })

    def last_n_closed_bars(self, sym: int, bt: int, p: int, n: int) -> List[dict]:
        ring = self._bars.get((sym, bt, p), [])
        return ring[-n:]


BTC, ETH = 1, 2
H4 = 14_400_000_000_000
M5 = 300_000_000_000


def test_grid_builds_full_cross_product() -> None:
    s = StubStrategy()
    g = grid(s, [BTC, ETH], [H4, M5]).ema(20)
    assert len(g) == 4
    keys = sorted(k for k, _ in g)
    assert keys == sorted([
        (BTC, TIME_BARS, H4), (BTC, TIME_BARS, M5),
        (ETH, TIME_BARS, H4), (ETH, TIME_BARS, M5),
    ])


def test_grid_lookup_by_two_or_three_tuple() -> None:
    s = StubStrategy()
    g = grid(s, [BTC], [H4]).ema(10)
    short = g[(BTC, H4)]
    long = g[(BTC, TIME_BARS, H4)]
    # Both lookups resolve to the same indicator instance.
    assert short is long


def test_grid_indicators_compute_independently() -> None:
    s = StubStrategy()
    for c in (100, 101, 102):
        s.push(BTC, TIME_BARS, H4, c)
    for c in (50, 51):
        s.push(ETH, TIME_BARS, H4, c)
    g = grid(s, [BTC, ETH], [H4]).sma(2)
    assert g[(BTC, H4)].is_ready()
    assert g[(ETH, H4)].is_ready()
    assert g[(BTC, H4)].value() == 101.5  # avg(101, 102)
    assert g[(ETH, H4)].value() == 50.5   # avg(50, 51)


def test_explicit_bar_type_param_tuple_in_timeframes() -> None:
    s = StubStrategy()
    s.push(BTC, 1, 100, 42.0)  # bar_type=Tick, param=100
    g = grid(s, [BTC], [(1, 100)]).close()
    assert g[(BTC, 1, 100)].is_ready()
    assert g[(BTC, 1, 100)].value() == 42.0


def test_grid_iteration_walks_every_cell() -> None:
    s = StubStrategy()
    g = grid(s, [BTC, ETH], [H4, M5]).ema(5)
    seen = []
    for key, ind in g:
        seen.append(key)
        assert hasattr(ind, "is_ready")
        assert hasattr(ind, "value")
    assert len(seen) == 4
