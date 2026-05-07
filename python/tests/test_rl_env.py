"""Tests for ``flox_py.rl_env.FloxTradingEnv``."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from typing import List, Tuple

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

from flox_py import rl_env  # noqa: E402


# A tiny synthetic tape: monotonic ramp so PnL signs are predictable.
_RAMP_TRADES: List[Tuple[int, float, float, int]] = [
    (1_000, 100.0, 1.0, 0),
    (2_000, 101.0, 1.0, 0),
    (3_000, 102.0, 1.0, 0),
    (4_000, 103.0, 1.0, 0),
    (5_000, 104.0, 1.0, 0),
    (6_000, 105.0, 1.0, 0),
]


class ConstructionTests(unittest.TestCase):
    def test_empty_trades_rejected(self) -> None:
        with self.assertRaises(ValueError):
            rl_env.FloxTradingEnv(trades=[])

    def test_invalid_qty_rejected(self) -> None:
        with self.assertRaises(ValueError):
            rl_env.FloxTradingEnv(trades=_RAMP_TRADES, qty=0)

    def test_invalid_window_rejected(self) -> None:
        with self.assertRaises(ValueError):
            rl_env.FloxTradingEnv(trades=_RAMP_TRADES, window_size=0)

    def test_action_space_is_three_discrete(self) -> None:
        env = rl_env.FloxTradingEnv(trades=_RAMP_TRADES)
        self.assertEqual(env.action_space.n, 3)
        self.assertTrue(env.action_space.contains(0))
        self.assertTrue(env.action_space.contains(2))
        self.assertFalse(env.action_space.contains(3))

    def test_observation_space_shape(self) -> None:
        env = rl_env.FloxTradingEnv(trades=_RAMP_TRADES, window_size=4)
        # window_size + position + unrealized = 6
        self.assertEqual(env.observation_space.shape, (6,))


class ResetTests(unittest.TestCase):
    def test_reset_returns_obs_and_info(self) -> None:
        env = rl_env.FloxTradingEnv(trades=_RAMP_TRADES, window_size=3)
        obs, info = env.reset()
        self.assertEqual(len(obs), 5)  # 3 prices + position + pnl
        self.assertEqual(info["step"], 0)
        self.assertEqual(info["position"], 0.0)

    def test_reset_clears_state_after_steps(self) -> None:
        env = rl_env.FloxTradingEnv(trades=_RAMP_TRADES, window_size=2)
        env.reset()
        env.step(1)  # go long
        env.step(0)
        env.reset()
        # After reset, position should be back to 0 and unrealized 0.
        obs, info = env.reset()
        self.assertEqual(info["position"], 0.0)
        self.assertEqual(obs[-2], 0.0)
        self.assertEqual(obs[-1], 0.0)


class StepTests(unittest.TestCase):
    def test_hold_yields_zero_reward(self) -> None:
        env = rl_env.FloxTradingEnv(trades=_RAMP_TRADES, window_size=2)
        env.reset()
        _, reward, terminated, truncated, info = env.step(0)
        self.assertAlmostEqual(reward, 0.0)
        self.assertEqual(info["position"], 0.0)
        self.assertFalse(terminated)
        self.assertFalse(truncated)

    def test_long_then_hold_in_uptrend_pays(self) -> None:
        env = rl_env.FloxTradingEnv(
            trades=_RAMP_TRADES, qty=1.0, window_size=2,
        )
        env.reset()
        # Go long at price 100; subsequent holds should produce
        # positive unrealized PnL deltas as the ramp climbs.
        _, r1, _, _, info1 = env.step(1)  # buy at 100
        _, r2, _, _, info2 = env.step(0)  # hold; price advances to 101
        _, r3, _, _, info3 = env.step(0)  # hold; price advances to 102
        self.assertEqual(info1["position"], 1.0)
        self.assertGreater(r2, 0.0)
        self.assertGreater(r3, 0.0)

    def test_short_then_hold_in_uptrend_loses(self) -> None:
        env = rl_env.FloxTradingEnv(
            trades=_RAMP_TRADES, qty=1.0, window_size=2,
        )
        env.reset()
        env.step(2)  # short at 100
        _, reward, _, _, info = env.step(0)
        self.assertEqual(info["position"], -1.0)
        self.assertLess(reward, 0.0)

    def test_truncated_when_tape_runs_out(self) -> None:
        env = rl_env.FloxTradingEnv(trades=_RAMP_TRADES, window_size=2)
        env.reset()
        truncated = False
        for _ in range(len(_RAMP_TRADES)):
            _, _, _, truncated, _ = env.step(0)
            if truncated:
                break
        self.assertTrue(truncated)

    def test_invalid_action_raises(self) -> None:
        env = rl_env.FloxTradingEnv(trades=_RAMP_TRADES)
        env.reset()
        with self.assertRaises(ValueError):
            env.step(5)

    def test_close_long_realizes_pnl(self) -> None:
        env = rl_env.FloxTradingEnv(
            trades=_RAMP_TRADES, qty=1.0, window_size=2,
        )
        env.reset()
        env.step(1)  # buy at 100
        env.step(0)  # price → 101
        env.step(0)  # price → 102
        # Close long by going short — realized PnL captured at close.
        _, _, _, _, info = env.step(2)
        self.assertGreaterEqual(info["realized_pnl"], 0.0)


class CustomRewardTests(unittest.TestCase):
    def test_custom_reward_fn_used(self) -> None:
        seen: List[dict] = []

        def reward(env, ctx) -> float:
            seen.append(dict(ctx))
            return 42.0

        env = rl_env.FloxTradingEnv(
            trades=_RAMP_TRADES, window_size=2, reward_fn=reward,
        )
        env.reset()
        _, r, _, _, _ = env.step(0)
        self.assertEqual(r, 42.0)
        self.assertEqual(len(seen), 1)
        self.assertIn("price", seen[0])


class FromTapeTests(unittest.TestCase):
    """Integration check: build a small tape then load it via
    from_tape. Mirrors the same recorder pattern other suites use."""

    def setUp(self) -> None:
        import shutil
        import tempfile
        import flox_py
        from flox_py import tape

        self.work = Path(tempfile.mkdtemp(prefix="rl-env-"))
        registry = flox_py.SymbolRegistry()
        sym = int(registry.add_symbol("rl", "BTCUSDT", tick_size=0.01))
        recorder = tape.make_recorder_hook(self.work)
        runner = flox_py.Runner(registry, on_signal=lambda _: None)
        runner.set_market_data_recorder(recorder)
        runner.start()
        for ts, price, qty, side in _RAMP_TRADES:
            runner.on_trade(sym, price, qty, side == 0, ts)
        runner.stop()
        recorder.close()
        self._cleanup = lambda: shutil.rmtree(self.work, ignore_errors=True)

    def tearDown(self) -> None:
        self._cleanup()

    def test_from_tape_loads_trades(self) -> None:
        env = rl_env.FloxTradingEnv.from_tape(self.work, window_size=3)
        self.assertEqual(len(env.trades), len(_RAMP_TRADES))
        obs, _ = env.reset()
        self.assertEqual(len(obs), 5)


if __name__ == "__main__":
    unittest.main()
