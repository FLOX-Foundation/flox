"""python/tests/test_renko_gap_bricks.py

Close semantics of the numpy batch-aggregation path -- doAggregate in
python/aggregator_bindings.h, the fourth copy of the close path and the only
one no C++ binary compiles. Renko gap synthesis first (the case this module
was written for), then the late-trade rule for time bars, which reaches this
code through the same template and is otherwise untested from Python.

`flox.aggregate_renko_bars` used to collapse a price gap spanning several
brick sizes into a single zero-range bar, silently dropping every brick in
between. This exercises the numpy batch-aggregation path (the one this
module binds directly, independent of the C++ event-driven aggregator) with
the same gap the C++ suite covers: a jump from 100 to 155 on a brick size of
10 (5.5 brick-widths) must come out as 5 bars walking cleanly from 100 to
150 in steps of 10 -- the brick that was forming closes at the first
boundary the gapping trade crossed (100 -> 110), then 4 synthesized bricks
carry the price to 150. The trailing bar left open at 150 is not returned: doAggregate (python/aggregator_bindings.h) never
flushes a still-open trailing bar for any policy, Renko included, so this
agrees with node/test/test_renko_gap_bricks.js on the same input.

Run from repo root:
    PYTHONPATH=build-audit/python python3 -m pytest python/tests/test_renko_gap_bricks.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-audit/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

import flox_py as flox  # noqa: E402

PRICE_SCALE = 1e8


class RenkoGapBricksTest(unittest.TestCase):
    def test_gap_past_several_bricks_synthesizes_the_missing_ones(self) -> None:
        ts = np.array([0, 1_000_000_000], dtype=np.int64)
        px = np.array([100.0, 155.0], dtype=np.float64)
        qty = np.array([1.0, 1.0], dtype=np.float64)
        is_buy = np.array([1, 1], dtype=np.uint8)

        bars = flox.aggregate_renko_bars(ts, px, qty, is_buy, brick_size=10.0)

        # doAggregate (python/aggregator_bindings.h) no longer flushes the
        # trailing, still-open bar for any policy -- see the batch bar-count
        # parity fix -- so this is the real bar (100 -> 110, closed at the
        # boundary the gapping trade crossed) plus 4 synthesized bricks, 5
        # bars total. No bar for the new brick left open at 150: that
        # matches node/test/test_renko_gap_bricks.js, which goes through the
        # same shared C ABI aggregator and never flushed a trailing bar to
        # begin with.
        self.assertEqual(len(bars), 5, "1 real + 4 synthesized bricks")

        opens = bars["open_raw"] / PRICE_SCALE
        closes = bars["close_raw"] / PRICE_SCALE
        highs = bars["high_raw"] / PRICE_SCALE
        lows = bars["low_raw"] / PRICE_SCALE

        np.testing.assert_allclose(opens, [100.0, 110.0, 120.0, 130.0, 140.0])
        np.testing.assert_allclose(closes, [110.0, 120.0, 130.0, 140.0, 150.0])
        np.testing.assert_allclose(highs, [110.0, 120.0, 130.0, 140.0, 150.0])
        np.testing.assert_allclose(lows, [100.0, 110.0, 120.0, 130.0, 140.0])

    def test_ordinary_single_brick_close_is_unaffected(self) -> None:
        ts = np.array([0, 1_000_000_000], dtype=np.int64)
        px = np.array([100.0, 114.0], dtype=np.float64)
        qty = np.array([1.0, 1.0], dtype=np.float64)
        is_buy = np.array([1, 1], dtype=np.uint8)

        bars = flox.aggregate_renko_bars(ts, px, qty, is_buy, brick_size=10.0)

        # One closed brick (100 -> 110); no bar for the trailing open one
        # at 110 -> 114 (doAggregate never flushes it -- see the test above).
        self.assertEqual(len(bars), 1)
        np.testing.assert_allclose(bars["open_raw"] / PRICE_SCALE, [100.0])
        np.testing.assert_allclose(bars["close_raw"] / PRICE_SCALE, [110.0])


class TimeBarLateTradeTest(unittest.TestCase):
    """A trade whose interval precedes the live bar's is dropped, not folded in.

    Timestamps are chosen so the result cannot depend on where the interval
    boundaries happen to fall: the two trades that belong to the live bar
    carry the *same* timestamp (so they are in the same bucket whatever the
    alignment), the late one sits 5 whole intervals behind it and the closing
    one 5 intervals ahead. Anything finer would be a coin flip -- flox_py maps
    unix nanoseconds onto an internal timebase whose origin is taken at import,
    so a bucket edge lands at an arbitrary offset inside the minute.
    """

    INTERVAL_S = 60.0
    LIVE_NS = 600_000_000_000     # the live bar's bucket
    LATE_NS = 300_000_000_000     # 5 intervals earlier: closed and gone
    NEXT_NS = 900_000_000_000     # 5 intervals later: closes the live bar

    def test_trade_from_a_closed_bucket_is_dropped(self) -> None:
        ts = np.array([self.LIVE_NS, self.LIVE_NS, self.LATE_NS, self.NEXT_NS], dtype=np.int64)
        px = np.array([100.0, 105.0, 90.0, 106.0], dtype=np.float64)
        qty = np.array([1.0, 1.0, 1.0, 1.0], dtype=np.float64)
        is_buy = np.array([1, 1, 1, 1], dtype=np.uint8)

        bars = flox.aggregate_time_bars(ts, px, qty, is_buy, interval_seconds=self.INTERVAL_S)

        # One closed bar; the trailing one opened by the 900s trade is never
        # flushed, as for every other policy on this path.
        self.assertEqual(len(bars), 1)
        self.assertAlmostEqual(bars["close_raw"][0] / PRICE_SCALE, 105.0,
                               msg="the late 90 must not become the close")
        self.assertAlmostEqual(bars["high_raw"][0] / PRICE_SCALE, 105.0)
        self.assertAlmostEqual(bars["low_raw"][0] / PRICE_SCALE, 100.0,
                               msg="the late 90 must not become the low either")
        self.assertEqual(bars["trade_count"][0], 2)
        self.assertAlmostEqual(bars["volume_raw"][0] / PRICE_SCALE, 205.0)

    def test_trade_from_the_live_bucket_is_folded(self) -> None:
        # The limit of the rule: only an *earlier* bucket is late. A trade
        # landing in the live bucket is ordinary data, whatever its arrival
        # order, and dropping it would cost the bar a real trade.
        ts = np.array([self.LIVE_NS, self.LIVE_NS, self.LIVE_NS, self.NEXT_NS], dtype=np.int64)
        px = np.array([100.0, 105.0, 103.0, 106.0], dtype=np.float64)
        qty = np.array([1.0, 1.0, 1.0, 1.0], dtype=np.float64)
        is_buy = np.array([1, 1, 1, 1], dtype=np.uint8)

        bars = flox.aggregate_time_bars(ts, px, qty, is_buy, interval_seconds=self.INTERVAL_S)

        self.assertEqual(len(bars), 1)
        self.assertAlmostEqual(bars["close_raw"][0] / PRICE_SCALE, 103.0)
        self.assertEqual(bars["trade_count"][0], 3)


if __name__ == "__main__":
    unittest.main()
