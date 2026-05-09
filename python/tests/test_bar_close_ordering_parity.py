"""Cross-binding parity test for bar-close dispatch ordering.

The C++ engine guarantees that on tied closes,
`MultiTimeframeAggregator` dispatches bars in the order their
timeframes were registered (see docs/explanation/bar-close-ordering.md
and tests/test_bar_close_ordering.cpp). This test pins the
pybind11 binding to the same rule. Sister tests in node, QuickJS,
and Codon assert the same outputs against the same C ABI.
"""
from __future__ import annotations

import flox_py


H4_NS = 4 * 60 * 60 * 1_000_000_000
H1_NS = 60 * 60 * 1_000_000_000
M5_NS = 5 * 60 * 1_000_000_000


def _drive(seconds_in_order: list[int]) -> list[int]:
    """Register timeframes in `seconds_in_order`, drive a tape across
    one H4 boundary, and return the dispatched bar params (in
    nanoseconds) at the tied close in dispatch order."""
    r = flox_py.BarDispatchRecorder()
    for s in seconds_in_order:
        r.add_time_interval_seconds(s)
    r.on_trade(symbol=1, price=100.0, qty=0.1, ts_ns=0)
    r.on_trade(symbol=1, price=101.0, qty=0.1, ts_ns=H4_NS)
    r.finalize()
    return [r.param_at(i) for i in range(r.count())]


def test_coarsest_first_registration_dispatches_coarsest_first() -> None:
    params = _drive([4 * 60 * 60, 60 * 60, 5 * 60])
    # First three entries are the tied-close dispatch in registration order.
    assert params[:3] == [H4_NS, H1_NS, M5_NS]


def test_reverse_registration_reverses_dispatch_order() -> None:
    params = _drive([5 * 60, 60 * 60, 4 * 60 * 60])
    assert params[:3] == [M5_NS, H1_NS, H4_NS]
