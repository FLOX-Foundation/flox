"""Tests for ``flox_py.latency_models``."""
from __future__ import annotations

import statistics
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

from flox_py import latency_models as lm  # noqa: E402


class ConstantTests(unittest.TestCase):
    def test_returns_configured_values(self) -> None:
        m = lm.ConstantLatency(feed_ns=100, order_ns=200, fill_ns=300)
        self.assertEqual(m.feed_delay(), 100)
        self.assertEqual(m.order_delay(), 200)
        self.assertEqual(m.fill_delay(), 300)

    def test_sample_returns_all_three_components(self) -> None:
        m = lm.ConstantLatency(feed_ns=10, order_ns=20, fill_ns=30)
        s = m.sample()
        self.assertEqual(s.feed_ns, 10)
        self.assertEqual(s.order_ns, 20)
        self.assertEqual(s.fill_ns, 30)

    def test_negative_values_rejected(self) -> None:
        with self.assertRaises(ValueError):
            lm.ConstantLatency(feed_ns=-1)

    def test_zero_default_is_instant(self) -> None:
        m = lm.ConstantLatency()
        self.assertEqual(m.sample().to_dict(),
                         {"feed_ns": 0, "order_ns": 0, "fill_ns": 0})


class GaussianTests(unittest.TestCase):
    def test_distribution_matches_expected_mean(self) -> None:
        m = lm.GaussianLatency(
            feed_mean_ns=1_000, feed_stddev_ns=100,
            order_mean_ns=2_000, order_stddev_ns=200,
            fill_mean_ns=3_000, fill_stddev_ns=300,
            seed=42,
        )
        n = 2_000
        feed = [m.feed_delay() for _ in range(n)]
        order = [m.order_delay() for _ in range(n)]
        fill = [m.fill_delay() for _ in range(n)]
        self.assertAlmostEqual(statistics.mean(feed), 1_000, delta=50)
        self.assertAlmostEqual(statistics.mean(order), 2_000, delta=80)
        self.assertAlmostEqual(statistics.mean(fill), 3_000, delta=120)

    def test_zero_stddev_returns_constant(self) -> None:
        m = lm.GaussianLatency(feed_mean_ns=500, feed_stddev_ns=0)
        for _ in range(20):
            self.assertEqual(m.feed_delay(), 500)

    def test_clamped_to_non_negative(self) -> None:
        m = lm.GaussianLatency(feed_mean_ns=1, feed_stddev_ns=1_000_000, seed=1)
        for _ in range(50):
            self.assertGreaterEqual(m.feed_delay(), 0)

    def test_seed_reproducible(self) -> None:
        a = lm.GaussianLatency(feed_mean_ns=1_000, feed_stddev_ns=100, seed=7)
        b = lm.GaussianLatency(feed_mean_ns=1_000, feed_stddev_ns=100, seed=7)
        for _ in range(20):
            self.assertEqual(a.feed_delay(), b.feed_delay())


class ExponentialTests(unittest.TestCase):
    def test_distribution_mean_close_to_expected(self) -> None:
        m = lm.ExponentialLatency(feed_mean_ns=1_000, seed=99)
        n = 5_000
        samples = [m.feed_delay() for _ in range(n)]
        # Exponential mean equals 1 / rate. With rate = 1/mean, the
        # sample mean should be close to mean. 5% tolerance is loose
        # but bounded.
        self.assertAlmostEqual(statistics.mean(samples), 1_000, delta=80)

    def test_zero_mean_returns_zero(self) -> None:
        m = lm.ExponentialLatency(feed_mean_ns=0, order_mean_ns=0, fill_mean_ns=0)
        for _ in range(20):
            self.assertEqual(m.sample().to_dict(),
                             {"feed_ns": 0, "order_ns": 0, "fill_ns": 0})

    def test_negative_mean_rejected(self) -> None:
        with self.assertRaises(ValueError):
            lm.ExponentialLatency(feed_mean_ns=-1)


class EmpiricalTests(unittest.TestCase):
    def test_only_returns_observed_values(self) -> None:
        m = lm.EmpiricalLatency(
            feed_samples=[100, 200, 300],
            order_samples=[10],
            fill_samples=[1, 2],
            seed=11,
        )
        seen_feed = {m.feed_delay() for _ in range(50)}
        seen_order = {m.order_delay() for _ in range(50)}
        seen_fill = {m.fill_delay() for _ in range(50)}
        self.assertTrue(seen_feed.issubset({100, 200, 300}))
        self.assertEqual(seen_order, {10})
        self.assertTrue(seen_fill.issubset({1, 2}))

    def test_empty_arrays_rejected(self) -> None:
        with self.assertRaises(ValueError):
            lm.EmpiricalLatency()

    def test_negative_value_rejected(self) -> None:
        with self.assertRaises(ValueError):
            lm.EmpiricalLatency(feed_samples=[100, -1])

    def test_calibrate_helper_returns_empirical(self) -> None:
        m = lm.calibrate_from_samples(
            feed_samples=[1, 2, 3],
            order_samples=[10, 20],
            fill_samples=[100],
        )
        self.assertIsInstance(m, lm.EmpiricalLatency)


class ResetTests(unittest.TestCase):
    def test_gaussian_reset_restarts_sequence(self) -> None:
        m = lm.GaussianLatency(feed_mean_ns=1_000, feed_stddev_ns=100, seed=5)
        first_run = [m.feed_delay() for _ in range(5)]
        m.reset(seed=5)
        second_run = [m.feed_delay() for _ in range(5)]
        self.assertEqual(first_run, second_run)


if __name__ == "__main__":
    unittest.main()
