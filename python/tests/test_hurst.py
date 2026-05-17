"""Tests for ``flox_py.hurst_dfa`` / ``flox_py.rolling_hurst``.

Validates DFA-Hurst on three canonical series:
- pure random walk (Hurst ≈ 0.5)
- positive AR(1) returns / persistent (Hurst > 0.55)
- negative AR(1) returns / anti-persistent (Hurst < 0.45)
"""
from __future__ import annotations

import math
import os
import sys
import unittest

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import numpy as np

import flox_py as flox  # noqa: E402


class HurstDfaTests(unittest.TestCase):

    def test_random_walk_near_half(self):
        rng = np.random.default_rng(42)
        returns = rng.normal(0.0, 0.01, 5000)
        h = flox.hurst_dfa(returns)
        # Random walk → H ≈ 0.5 ± ~0.05 finite-sample noise
        self.assertGreater(h, 0.40)
        self.assertLess(h, 0.60)

    def test_persistent_ar1_positive(self):
        rng = np.random.default_rng(7)
        shocks = rng.normal(0.0, 1.0, 5000)
        x = np.zeros(5000)
        x[0] = shocks[0]
        for i in range(1, 5000):
            x[i] = 0.6 * x[i - 1] + shocks[i]
        x *= 0.01
        h = flox.hurst_dfa(x)
        # AR(1) with phi=+0.6 → strongly persistent
        self.assertGreater(h, 0.55)

    def test_anti_persistent_ar1_negative(self):
        rng = np.random.default_rng(11)
        shocks = rng.normal(0.0, 1.0, 5000)
        x = np.zeros(5000)
        x[0] = shocks[0]
        for i in range(1, 5000):
            x[i] = -0.6 * x[i - 1] + shocks[i]
        x *= 0.01
        h = flox.hurst_dfa(x)
        # AR(1) with phi=-0.6 → strongly anti-persistent
        self.assertLess(h, 0.45)

    def test_returns_nan_on_short_input(self):
        h = flox.hurst_dfa(np.zeros(10))
        self.assertTrue(math.isnan(h))

    def test_custom_scales(self):
        rng = np.random.default_rng(3)
        returns = rng.normal(0.0, 0.01, 5000)
        # Explicit subset of scales — should still return a finite value
        # in the same ballpark as defaults.
        h = flox.hurst_dfa(returns, scales=np.array([10, 25, 50, 100, 250], dtype=np.int64))
        self.assertTrue(math.isfinite(h))
        self.assertGreater(h, 0.30)
        self.assertLess(h, 0.70)


class RollingHurstTests(unittest.TestCase):

    def test_prefix_nans_then_finite(self):
        rng = np.random.default_rng(123)
        prices = np.cumprod(1.0 + 0.001 * rng.normal(0, 1, 2000))
        window = 500
        out = flox.rolling_hurst(prices, window=window)
        self.assertEqual(out.shape[0], prices.shape[0])
        # First `window+1` outputs must be NaN — insufficient history.
        self.assertTrue(np.all(np.isnan(out[: window + 1])))
        # After warmup at least some values are finite.
        self.assertGreater(np.sum(np.isfinite(out)), 0)

    def test_random_walk_rolling_near_half(self):
        rng = np.random.default_rng(99)
        prices = np.cumprod(1.0 + 0.001 * rng.normal(0, 1, 3000))
        out = flox.rolling_hurst(prices, window=1080)
        finite = out[np.isfinite(out)]
        self.assertGreater(finite.size, 0)
        # Median rolling Hurst on a random walk should be close to 0.5.
        self.assertGreater(float(np.median(finite)), 0.40)
        self.assertLess(float(np.median(finite)), 0.60)


if __name__ == "__main__":
    unittest.main(verbosity=2)
