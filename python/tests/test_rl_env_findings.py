"""python/tests/test_rl_env_findings.py

Regression tests for four findings in ``flox_py.rl_env``:

* Observation leak -- the observation's unrealized-PnL slot (and the open-order
  distance slot) was built from ``trades[self._idx]``, the price of
  the NEXT trade the agent's action for this step has not been
  executed against yet, instead of the last trade actually replayed.
  On a tape where the next move is not derivable from the price
  history but the current price fully determines it (a zigzag), an
  agent can recover the leaked price from the slot with zero error and
  trade it almost perfectly.
* Venue-stack entry price -- in venue-stack mode the PnL slot used ``self._entry_price``,
  which is only ever written on the bare-fill path; venue-stack fills
  update ``self._venue_entry_price`` instead, so the slot silently
  became ``price * position`` rather than unrealized PnL.
* Reset does not clear the venue stack -- ``reset()`` did not reset the venue stack: the account kept
  its position and equity, and the executor kept accumulating fills,
  across every episode boundary -- exactly where the standard RL
  training loop (``PPO(...).learn(...)``) resets the env.
* Sharpe scales with capital -- the reported Sharpe ratio divided a per-step reward (a
  dollar amount) by the standard deviation of the equity LEVEL (which
  tracks the account's starting balance) instead of the standard
  deviation of the reward series, so the same strategy on the same
  tape reported a Sharpe that scaled with 1 / starting_capital.

Run from repo root:
    PYTHONPATH=build/python python3 -m pytest python/tests/test_rl_env_findings.py
"""
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

import flox_py  # noqa: E402
from flox_py import rl_env  # noqa: E402


def _zigzag_tape(n: int, amplitude: float = 1.0, base: float = 100.0) -> List[Tuple[int, float, float, int]]:
    """A tape where the price at an odd step is fully determined by
    the price at the preceding even step (it mirrors back to `base`),
    but is not derivable from anything earlier than that -- the
    minimal series that discriminates "the agent saw the current
    price" from "the agent saw the next trade's price" (a monotonic
    ramp or a random walk cannot: a random walk was tried and rejected during manual verification because
    the first reviewer's random-walk tape produced no signal at all).
    """
    trades = []
    price = base
    sign = 1.0
    for i in range(n):
        if i % 2 == 1:
            price = base + sign * amplitude
            sign = -sign
        else:
            price = base
        trades.append((1000 * (i + 1), price, 1.0, 0))
    return trades


class ObservationLeakTests(unittest.TestCase):
    """the bare-fill path's observation must be built from the
    price already replayed, never from the trade the next step has not
    happened yet."""

    def test_pnl_slot_uses_last_known_price_not_next_trade(self) -> None:
        tape = _zigzag_tape(6)
        env = rl_env.FloxTradingEnv(trades=tape, qty=1.0, window_size=1)
        env.reset()
        # Step 0: hold. Step 1: go long at trade[1]'s price.
        env.step(0)
        obs, reward, terminated, truncated, info = env.step(1)
        # obs layout: [price_window..., position, unrealized_pnl, *slots]
        # window_size=1, no open-order slots (bare path) -> index -2 is
        # the unrealized_pnl slot (index -1 is position... actually
        # order is [window..., position, unreal]).
        position_slot = obs[-2]
        pnl_slot = obs[-1]
        self.assertEqual(position_slot, 1.0)

        entry_price = info["price"]  # the price this step's fill happened at
        last_known_price = tape[1][1]  # == entry_price, since this step just filled
        next_trade_price = tape[2][1]  # NOT yet replayed

        honest_pnl = (last_known_price - entry_price) * 1.0
        leaked_pnl = (next_trade_price - entry_price) * 1.0

        self.assertAlmostEqual(pnl_slot, honest_pnl, places=9)
        if abs(leaked_pnl - honest_pnl) > 1e-9:
            self.assertNotAlmostEqual(pnl_slot, leaked_pnl, places=6)

    def test_open_order_distance_slot_uses_last_known_price(self) -> None:
        # Venue-stack path, since only it carries open-order slots.
        stack = flox_py.VenueStack.binance_um_futures(account_id=1, equity=10_000.0)
        tape = _zigzag_tape(6, base=100.0, amplitude=20.0)
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=tape, qty=1.0, window_size=1, symbol_id=1,
            action_mode="continuous", n_open_slots=1, tick_size=1.0,
        )
        env.reset()
        # Submit a limit BUY 10 ticks BELOW mid (negative offset) so it
        # rests instead of filling immediately, leaving a slot to
        # inspect after the step.
        obs, reward, terminated, truncated, info = env.step((1.0, -10.0, 1.0))
        if not env._open_orders:
            self.skipTest("order filled immediately on this synthetic tape")
        last_known_price = env._last_known_price()
        # slot layout: [window..., position, unreal, qty, age, distance, queue]
        distance_slot = obs[-2]
        od = next(iter(env._open_orders.values()))
        expected_distance = (od["price"] - last_known_price) / env.tick_size
        expected_distance = max(-1.0, min(1.0, expected_distance / max(int(env.max_price_offset_ticks), 1)))
        self.assertAlmostEqual(distance_slot, expected_distance, places=6)


class VenueEntryPriceObservationTests(unittest.TestCase):
    """the venue-stack path's PnL slot must track
    `_venue_entry_price` (what `_apply_fill` actually maintains), not
    `_entry_price` (which stays 0 in this mode)."""

    def test_pnl_slot_matches_venue_entry_price_not_zero(self) -> None:
        stack = flox_py.VenueStack.binance_um_futures(account_id=1, equity=10_000.0)
        tape = [
            (1_000, 100.0, 1.0, 0),
            (2_000, 110.0, 1.0, 0),
            (3_000, 110.0, 1.0, 0),
        ]
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=tape, qty=1.0, window_size=1, symbol_id=1,
            action_mode="discrete", n_open_slots=0,
        )
        env.reset()
        env.step(1)  # market buy at ~100
        obs, reward, terminated, truncated, info = env.step(0)  # hold, price now 110
        # obs layout with n_open_slots=0: [window(1), position, unreal]
        pnl_slot = obs[-1]
        # Before the fix this slot was `last_price * position` (raw
        # price, ~110), not unrealized PnL (~10). The two are only
        # equal by coincidence, so this assertion is discriminating.
        self.assertLess(abs(pnl_slot), 50.0)
        self.assertAlmostEqual(
            pnl_slot, (env._last_known_price() - env._venue_entry_price) * env._position,
            places=6,
        )


class ResetClearsVenueStackTests(unittest.TestCase):
    """reset() must restore the venue stack's account to its
    starting equity and clear accumulated fills, or repeated episodes
    on the same tape (the standard RL training loop) silently drift."""

    def _tape(self, n: int = 40) -> List[Tuple[int, float, float, int]]:
        trades = []
        price = 100.0
        for i in range(n):
            price += 0.5 if i % 2 == 0 else -0.5
            trades.append((1000 * (i + 1), price, 1.0, i % 2))
        return trades

    def test_repeated_episodes_do_not_drift_equity(self) -> None:
        stack = flox_py.VenueStack.binance_um_futures(account_id=1, equity=100_000.0)
        tape = self._tape()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=tape, qty=1.0, window_size=2, symbol_id=1,
            action_mode="discrete",
        )
        starting_equity = stack.account().equity()
        episode_returns = []
        policy = [1, 0, 2, 0] * (len(tape) // 4 + 1)
        n_episodes = 10
        for _ in range(n_episodes):
            env.reset()
            # A fresh episode must start with the account back at its
            # original equity, no open position, and no leftover
            # fills -- this is the specific mechanism the report
            # measured: equity fell 27% and fills kept accumulating
            # over 50 "identical" episodes purely from state bleeding
            # across the reset boundary.
            self.assertAlmostEqual(stack.account().equity(), starting_equity, places=6)
            self.assertEqual(stack.account().position_count(), 0)
            self.assertEqual(stack.executor().fill_count, 0)

            total_reward = 0.0
            terminated = truncated = False
            for action in policy:
                if terminated or truncated:
                    break
                _, reward, terminated, truncated, _ = env.step(action)
                total_reward += reward
            episode_returns.append(total_reward)

            # And after the episode, the account must be back to a
            # position-free, fully accounted state -- nothing "won"
            # or "lost" money the policy's actions did not cause and
            # then failed to close out.
            equity_now = stack.account().equity()
            self.assertLess(
                abs(equity_now - starting_equity), starting_equity * 0.05,
                f"equity drifted from {starting_equity} to {equity_now} "
                f"purely from repeated episodes on the same tape",
            )

        # Same policy, same tape, every episode: the report's measured
        # signature was episode-N reward magnitude growing ~linearly
        # with N (24x by episode 50, from a position and its PnL
        # compounding across resets that never actually cleared). With
        # the account and fills reset, that growth trend must be gone
        # -- the last episode's return must not dwarf the first's.
        # (Small residual variation between episodes is expected and
        # accepted: SimulatedExecutor's own order-matching/queue-
        # position state has no reset of its own yet, so a market
        # fill's simulated price can differ by a tick between two
        # otherwise-identical episodes -- see the comment on
        # `_reset_venue_stack`.)
        first, last = episode_returns[0], episode_returns[-1]
        if abs(first) > 1e-9:
            growth_ratio = abs(last) / abs(first)
            self.assertLess(
                growth_ratio, 3.0,
                f"episode returns {episode_returns}: growth ratio "
                f"{growth_ratio:.2f}x looks like the pre-fix drift, not "
                f"per-episode execution noise",
            )


class SharpeScaleInvarianceTests(unittest.TestCase):
    """the reported Sharpe must not depend on the account's
    starting equity."""

    NS_PER_DAY = 86_400 * 1_000_000_000

    def _tape(self, n_days: int = 8) -> List[Tuple[int, float, float, int]]:
        trades = []
        price = 50_000.0
        for d in range(n_days):
            for h in range(24):
                ts = d * self.NS_PER_DAY + h * 3600 * 1_000_000_000
                price += 3.0 if h % 3 == 0 else -1.0
                trades.append((ts, price, 1.0, h % 2))
        return trades

    def _run_sharpe(self, initial_equity: float) -> float:
        class AlternatingModel:
            """Deterministic non-trivial policy: alternates signed
            exposure so the reward series has real variance -- an
            always-hold policy has zero reward and would not
            discriminate the bug (see the note in FINDINGS about the
            first reviewer's non-differentiating test)."""

            def __init__(self) -> None:
                self._t = 0

            def predict(self, obs, deterministic=True):
                self._t += 1
                signed = 1.0 if (self._t // 5) % 2 == 0 else -1.0
                return [signed, 0.0, 0.0], None

        wf = rl_env.WalkForwardRL(
            venue_stack_factory=lambda: flox_py.VenueStack.binance_um_futures(
                1, initial_equity
            ),
            tape=self._tape(),
            train_window_days=4,
            test_window_days=2,
            n_folds=2,
            env_kwargs={
                "qty": 0.01, "max_position": 0.02, "window_size": 4,
                "tick_size": 0.5, "n_open_slots": 0,
            },
        )
        model = AlternatingModel()
        for _train_env, test_env in wf:
            wf.evaluate(model, test_env)
        agg = wf.aggregate()
        return agg["mean_sharpe"]

    def test_sharpe_is_independent_of_starting_capital(self) -> None:
        sharpe_small = self._run_sharpe(10_000.0)
        sharpe_large = self._run_sharpe(1_000_000.0)
        if sharpe_small == 0.0 and sharpe_large == 0.0:
            self.skipTest("policy produced no realized reward on this synthetic tape")
        # Before the fix, changing only the starting capital 10k -> 1M
        # moved the reported Sharpe by exactly 100x (equity ~ capital,
        # so std(equity) scales with capital while mean(reward) does
        # not). After the fix the ratio must be close to 1.
        ratio = sharpe_large / sharpe_small if sharpe_small != 0 else float("inf")
        self.assertGreater(ratio, 0.5)
        self.assertLess(ratio, 2.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
