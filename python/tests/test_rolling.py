"""Tests for ``flox_py.rolling`` — sliding top-K threshold helper.

Covers:

  * Output matches the naive Python loop bar-for-bar on a small array.
  * Warmup window writes NaN for the first ``window`` slots.
  * Edge cases: ``window > len(values)``, ``k > window``, ``k == 1``
    matches plain ``rolling_max``.
  * Performance budget: 17,520-bar / window=720 / k=3 finishes in
    well under 1 second (the budget in the tracker is 100 ms; the
    test asserts a generous 2 s envelope so a noisy laptop / CI box
    doesn't flake).

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_rolling.py
"""
from __future__ import annotations

import sys
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

from flox_py import rolling  # noqa: E402


def _naive_top_k(values: np.ndarray, window: int, k: int) -> np.ndarray:
    """Reference implementation: the exact loop the helper replaces."""
    n = values.size
    out = np.full(n, np.nan, dtype=np.float64)
    for i in range(window, n):
        win = values[i - window : i]
        # np.partition(..., -k)[-k] is the (k-th largest) in ascending order.
        out[i] = float(np.partition(win, -k)[-k])
    return out


class TopKThresholdTests(unittest.TestCase):
    def test_matches_naive_loop(self) -> None:
        rng = np.random.default_rng(0)
        values = rng.standard_normal(500)
        for window, k in [(50, 1), (50, 3), (100, 5), (200, 1)]:
            ref = _naive_top_k(values, window=window, k=k)
            got = rolling.top_k_threshold(values, window=window, k=k)
            np.testing.assert_allclose(
                got[window:], ref[window:],
                err_msg=f"window={window} k={k}",
            )
            self.assertTrue(np.isnan(got[:window]).all(),
                            f"warmup should be NaN, window={window}")

    def test_warmup_is_nan(self) -> None:
        out = rolling.top_k_threshold(np.arange(100, dtype=np.float64),
                                      window=10, k=3)
        self.assertTrue(np.isnan(out[:10]).all())
        self.assertFalse(np.isnan(out[10:]).any())

    def test_window_larger_than_input(self) -> None:
        # Helper short-circuits to all-NaN without raising.
        out = rolling.top_k_threshold(np.arange(5, dtype=np.float64),
                                      window=10, k=1)
        self.assertEqual(out.size, 5)
        self.assertTrue(np.isnan(out).all())

    def test_k_equals_one_matches_rolling_max(self) -> None:
        rng = np.random.default_rng(1)
        values = rng.standard_normal(200)
        a = rolling.top_k_threshold(values, window=30, k=1)
        b = rolling.rolling_max(values, window=30)
        np.testing.assert_array_equal(a, b)
        # k=1 ⇒ rolling max of the trailing window.
        for i in range(30, values.size):
            self.assertEqual(float(a[i]), float(values[i - 30 : i].max()))

    def test_bad_inputs_raise(self) -> None:
        v = np.arange(50, dtype=np.float64)
        with self.assertRaises(ValueError):
            rolling.top_k_threshold(v, window=0, k=1)
        with self.assertRaises(ValueError):
            rolling.top_k_threshold(v, window=10, k=0)
        with self.assertRaises(ValueError):
            rolling.top_k_threshold(v, window=10, k=11)
        with self.assertRaises(ValueError):
            rolling.top_k_threshold(np.ones((3, 3)), window=2, k=1)

    def test_perf_budget(self) -> None:
        # 17,520 bars = BTC 1h × 2y. Bench in the tracker is < 100 ms;
        # CI gets a 2 s envelope to absorb noisy boxes without flaking.
        rng = np.random.default_rng(42)
        values = rng.standard_normal(17_520)
        t0 = time.perf_counter()
        out = rolling.top_k_threshold(values, window=720, k=3)
        dt = time.perf_counter() - t0
        self.assertLess(dt, 2.0, f"top_k_threshold took {dt*1000:.1f} ms")
        # And the output is well-formed.
        self.assertEqual(out.size, 17_520)
        self.assertTrue(np.isnan(out[:720]).all())
        self.assertFalse(np.isnan(out[720:]).any())


if __name__ == "__main__":
    unittest.main()
