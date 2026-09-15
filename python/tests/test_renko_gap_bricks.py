"""python/tests/test_renko_gap_bricks.py

`flox.aggregate_renko_bars` used to collapse a price gap spanning several
brick sizes into a single zero-range bar, silently dropping every brick in
between. This exercises the numpy batch-aggregation path (the one this
module binds directly, independent of the C++ event-driven aggregator) with
the same gap the C++ suite covers: a jump from 100 to 155 on a brick size of
10 (5.5 brick-widths) must come out as the real bar plus the 4 complete
bricks a continuous price path would have produced -- 5 bars in total,
walking cleanly from 100 to 150 in steps of 10. The trailing bar left open
at 155 is not returned: doAggregate (python/aggregator_bindings.h) never
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
        # parity fix -- so this is the real bar (100 -> 100, no trade ever
        # touched a price in between) plus 4 synthesized bricks, 5 bars
        # total. No bar for the new brick left open at the raw trade price
        # (155): that matches node/test/test_renko_gap_bricks.js, which
        # goes through the same shared C ABI aggregator and never flushed
        # a trailing bar to begin with.
        self.assertEqual(len(bars), 5, "1 real + 4 synthesized bricks")

        opens = bars["open_raw"] / PRICE_SCALE
        closes = bars["close_raw"] / PRICE_SCALE
        highs = bars["high_raw"] / PRICE_SCALE
        lows = bars["low_raw"] / PRICE_SCALE

        np.testing.assert_allclose(opens, [100.0, 110.0, 120.0, 130.0, 140.0])
        np.testing.assert_allclose(closes, [100.0, 120.0, 130.0, 140.0, 150.0])
        np.testing.assert_allclose(highs, [100.0, 120.0, 130.0, 140.0, 150.0])
        np.testing.assert_allclose(lows, [100.0, 110.0, 120.0, 130.0, 140.0])

    def test_ordinary_single_brick_close_is_unaffected(self) -> None:
        ts = np.array([0, 1_000_000_000], dtype=np.int64)
        px = np.array([100.0, 114.0], dtype=np.float64)
        qty = np.array([1.0, 1.0], dtype=np.float64)
        is_buy = np.array([1, 1], dtype=np.uint8)

        bars = flox.aggregate_renko_bars(ts, px, qty, is_buy, brick_size=10.0)

        # One closed brick (100 -> 100); no bar for the trailing open one
        # at 114 (doAggregate never flushes it -- see the test above).
        self.assertEqual(len(bars), 1)
        opens = bars["open_raw"] / PRICE_SCALE
        np.testing.assert_allclose(opens, [100.0])


if __name__ == "__main__":
    unittest.main()
