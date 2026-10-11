"""Time bars from real timestamps start on wall-clock boundaries.

`lrvx.aggregate_time_bars` carries trade timestamps through the engine's
steady-clock domain, offset from Unix time by a per-process constant. The
bar policies used to snap bucket starts in that domain, so a 60 s bar from
a tape with real exchange timestamps started some seconds into the minute,
and a different number of seconds in the next process. The Python module
anchors the offset at import, so this test sees the real mapping.
"""
from __future__ import annotations

import numpy as np

import lrvx

NS = 1_000_000_000
MINUTE_NS = 60 * NS
# 2026-09-01T00:00:20Z
BASE_NS = 1_788_220_820 * NS


def _bars(interval_s: float, offsets_s):
    ts = np.array([BASE_NS + int(o * NS) for o in offsets_s], dtype=np.int64)
    px = np.linspace(100.0, 100.0 + len(ts) - 1, len(ts))
    qty = np.ones(len(ts), dtype=np.float64)
    is_buy = np.ones(len(ts), dtype=np.uint8)
    return lrvx.aggregate_time_bars(ts, px, qty, is_buy, interval_s)


def test_minute_bars_start_on_the_minute():
    # 00:00:20, 00:00:50 | 00:01:05, 00:01:30 | 00:02:10 (opens the third)
    bars = _bars(60.0, [0, 30, 45, 70, 110])
    assert len(bars) >= 2, bars
    minute0 = BASE_NS - 20 * NS
    starts = [int(b["start_time_ns"]) for b in bars]
    ends = [int(b["end_time_ns"]) for b in bars]
    assert starts[:2] == [minute0, minute0 + MINUTE_NS], (
        f"bar starts are {[(s % MINUTE_NS) / NS for s in starts]} s past the minute"
    )
    assert ends[:2] == [minute0 + MINUTE_NS, minute0 + 2 * MINUTE_NS]
    for s in starts:
        assert s % MINUTE_NS == 0
    # 00:01:05 belongs to the second bar, not the first.
    assert float(bars[0]["close_raw"]) / 1e8 == 101.0
    assert float(bars[1]["open_raw"]) / 1e8 == 102.0


def test_five_second_bars_align_too():
    bars = _bars(5.0, [0, 1, 4, 6, 11, 16])
    assert len(bars) >= 3
    for b in bars:
        s, e = int(b["start_time_ns"]), int(b["end_time_ns"])
        assert s % (5 * NS) == 0, s
        assert e - s == 5 * NS
