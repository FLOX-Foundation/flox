"""Tests for `flox_py.whites_reality_check`.

White's reality check is a bootstrap p-value, so the assertions are
distribution-style rather than equality-style:

* When one strategy has a clearly positive mean return and the rest
  are noise, the p-value should be small (signal is real).
* When every strategy is pure noise, the p-value should be uniform-ish
  on (0, 1) — definitely not close to zero on average.
* The best_index should always pick the highest-mean strategy.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
# Prefer a freshly built compiled extension under build/python/ over the
# installed wheel; this mirrors how every other binding test is run.
for candidate in ("build/python", "build-py312/python"):
    p = REPO_ROOT / candidate
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import flox_py as flox  # noqa: E402


def _fixed_returns(n_strategies: int, n_periods: int, *, seed: int = 7,
                   signal: float = 0.0, signal_strategy: int = -1) -> np.ndarray:
    """Build a (K, T) returns matrix with optional signal injection."""
    rng = np.random.default_rng(seed)
    out = rng.standard_normal((n_strategies, n_periods)) * 0.01
    if signal_strategy >= 0:
        out[signal_strategy] += signal
    return out


class WhitesRealityCheckTests(unittest.TestCase):

    def test_signal_strategy_has_low_p_value(self):
        # Inject a +0.005 mean (≈ +0.5 sigma per period given std=0.01)
        # for one of 5 strategies.
        rets = _fixed_returns(5, 252, signal=0.005, signal_strategy=2)
        out = flox.whites_reality_check(
            rets, num_bootstrap=2000, avg_block_size=0.0, seed=42)
        self.assertEqual(out["best_index"], 2)
        self.assertGreater(out["best_stat"], 0.0)
        self.assertLess(out["p_value"], 0.05,
                        f"expected significance, got p={out['p_value']!r}")

    def test_pure_noise_does_not_yield_low_p(self):
        # All 5 strategies are pure noise → the p-value should be high
        # most of the time. Run with a fixed seed; assert it's well
        # above the 0.05 threshold so the test is stable.
        rets = _fixed_returns(5, 252, seed=11)
        out = flox.whites_reality_check(
            rets, num_bootstrap=2000, avg_block_size=0.0, seed=42)
        self.assertGreater(
            out["p_value"], 0.10,
            f"pure noise should NOT look significant; got "
            f"p={out['p_value']!r}, best_stat={out['best_stat']!r}",
        )

    def test_more_strategies_softens_the_test(self):
        """Adding more noise strategies to a fixed signal should make
        the p-value larger (multiple-comparison penalty)."""
        rng_seed = 19
        signal = 0.003
        rets_few = _fixed_returns(3, 252, seed=rng_seed,
                                   signal=signal, signal_strategy=0)
        rets_many = _fixed_returns(20, 252, seed=rng_seed,
                                    signal=signal, signal_strategy=0)
        # Reusing seed=rng_seed regenerates the noise array; the wider
        # one has 17 extra "lottery tickets" for false positives, so
        # the p-value can only be ≥ the narrower case.
        p_few = flox.whites_reality_check(
            rets_few, num_bootstrap=2000, seed=42)["p_value"]
        p_many = flox.whites_reality_check(
            rets_many, num_bootstrap=2000, seed=42)["p_value"]
        self.assertGreaterEqual(p_many, p_few - 0.02,
                                 f"multi-comparison penalty broken: "
                                 f"p_few={p_few}, p_many={p_many}")

    def test_returns_shape_validation(self):
        with self.assertRaises(Exception):
            flox.whites_reality_check(np.zeros(5))  # 1D — not allowed
        with self.assertRaises(Exception):
            flox.whites_reality_check(np.zeros((0, 100)))
        with self.assertRaises(Exception):
            flox.whites_reality_check(np.zeros((3, 0)))

    def test_seed_is_deterministic(self):
        rets = _fixed_returns(4, 200, seed=23,
                              signal=0.002, signal_strategy=1)
        a = flox.whites_reality_check(rets, num_bootstrap=1000, seed=99)
        b = flox.whites_reality_check(rets, num_bootstrap=1000, seed=99)
        self.assertEqual(a["p_value"], b["p_value"])
        self.assertEqual(a["best_stat"], b["best_stat"])
        self.assertEqual(a["best_index"], b["best_index"])

    def test_avg_block_size_auto_vs_explicit(self):
        rets = _fixed_returns(4, 200, seed=29,
                              signal=0.003, signal_strategy=2)
        auto = flox.whites_reality_check(rets, num_bootstrap=1000,
                                          avg_block_size=0.0, seed=42)
        explicit = flox.whites_reality_check(rets, num_bootstrap=1000,
                                              avg_block_size=10.0, seed=42)
        # Both should pick the same strategy and produce a finite,
        # in-range p-value. Numerics may differ, that's fine.
        self.assertEqual(auto["best_index"], 2)
        self.assertEqual(explicit["best_index"], 2)
        for r in (auto, explicit):
            self.assertGreaterEqual(r["p_value"], 0.0)
            self.assertLessEqual(r["p_value"], 1.0)


if __name__ == "__main__":
    unittest.main()
