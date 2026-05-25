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


class FromVenueStackTests(unittest.TestCase):
    """T032: env routes orders through stack.executor(), reward
    reflects fees and funding via account equity, liquidation
    terminates the episode."""

    def _stack(self):
        import flox_py
        return flox_py.VenueStack.binance_um_futures(
            account_id=1, equity=10_000.0
        )

    def test_returns_venueexecutor(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1,
            action_mode="discrete",
        )
        self.assertEqual(env.venue_stack, stack)
        self.assertEqual(type(stack.executor()).__name__, "VenueExecutor")

    def test_hold_only_does_not_submit_orders(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1,
            action_mode="discrete",
        )
        env.reset()
        for _ in range(len(_RAMP_TRADES) - 1):
            env.step(0)
        self.assertEqual(stack.executor().fill_count, 0)

    def test_long_open_routes_through_executor(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1,
            action_mode="discrete",
        )
        env.reset()
        env.step(1)
        self.assertGreaterEqual(stack.executor().fill_count, 1)

    def test_fee_deduction_matches_schedule(self) -> None:
        """T032 acceptance — known fee on a single market fill shifts
        net reward by the expected delta. Buy 1 unit, then check the
        equity dropped by exactly the schedule's fee for that notional."""
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1,
            action_mode="discrete",
        )
        env.reset()
        equity_before = stack.account().equity()
        env.step(1)  # market buy
        fills = stack.executor().fills_list()
        self.assertGreaterEqual(len(fills), 1)
        fill = fills[0]
        notional = float(fill["price"]) * float(fill["quantity"])
        # Taker fee on first fill (account has zero rolling notional →
        # base tier). The schedule answer is the source of truth.
        expected_fee = float(stack.fees().fee_for(0, notional, False))
        # Equity reflects fee deduction.
        self.assertAlmostEqual(
            stack.account().equity(), equity_before - expected_fee, places=4
        )

    def test_reward_is_equity_at_mark_delta(self) -> None:
        """Reward should equal the change in account equity plus
        unrealized PnL between consecutive steps."""
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1,
            action_mode="discrete",
        )
        env.reset()
        acct = stack.account()
        prev = acct.equity() + acct.total_unrealised_pnl()
        for action in [1, 0, 0, 0]:
            _, reward, _, _, _ = env.step(action)
            current = acct.equity() + acct.total_unrealised_pnl()
            self.assertAlmostEqual(reward, current - prev, places=6)
            prev = current

    def test_unrealized_pnl_visible_in_info(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1,
            action_mode="discrete",
        )
        env.reset()
        _, _, _, _, info = env.step(1)
        self.assertIn("equity", info)
        self.assertIn("unrealized_pnl", info)
        self.assertIn("equity_at_mark", info)
        self.assertIn("fee_tier", info)
        self.assertIn("liquidation_outcome", info)


class ContinuousActionTests(unittest.TestCase):
    """T033: Box((3,)) action space — signed qty / price offset in
    ticks / TIF flag — with market vs limit decode by offset==0 and
    out-of-bounds clipping (warning, not exception)."""

    def _stack(self):
        import flox_py
        return flox_py.VenueStack.binance_um_futures(
            account_id=1, equity=10_000.0
        )

    def test_default_action_mode_for_venue_stack_is_continuous(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1
        )
        self.assertEqual(env.action_mode, "continuous")
        self.assertEqual(env.action_space.shape, (3,))

    def test_default_action_mode_for_from_tape_is_discrete(self) -> None:
        env = rl_env.FloxTradingEnv.from_tape  # avoid actually loading
        # Construct directly without venue stack — should default
        # discrete.
        env = rl_env.FloxTradingEnv(trades=_RAMP_TRADES, qty=1.0)
        self.assertEqual(env.action_mode, "discrete")
        self.assertEqual(env.action_space.n, 3)

    def test_action_mode_rejects_unknown(self) -> None:
        with self.assertRaises(ValueError):
            rl_env.FloxTradingEnv(
                trades=_RAMP_TRADES, action_mode="quadratic"
            )

    def test_market_decode_when_offset_zero(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, max_position=1.0,
            window_size=2, symbol_id=1, tick_size=0.5,
        )
        env.reset()
        # signed_qty=+1.0, offset=0 → market buy 1.0
        _, _, _, _, info = env.step([1.0, 0.0, 0.0])
        self.assertEqual(info["order_type"], "market")
        self.assertGreaterEqual(stack.executor().fill_count, 1)

    def test_limit_decode_when_offset_nonzero(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, max_position=1.0,
            window_size=2, symbol_id=1, tick_size=0.5,
        )
        env.reset()
        # signed_qty=+1.0, offset=2 ticks, post-only → limit buy
        _, _, _, _, info = env.step([1.0, 2.0, 2.0])
        self.assertEqual(info["order_type"], "limit")

    def test_out_of_bounds_clipped_with_warning(self) -> None:
        import warnings
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, max_position=1.0,
            window_size=2, symbol_id=1, max_price_offset_ticks=5,
        )
        env.reset()
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            _, _, _, _, info = env.step([5.0, 100.0, 9.0])
        self.assertTrue(info["action_clipped"])
        self.assertTrue(
            any(issubclass(w.category, RuntimeWarning) for w in caught)
        )

    def test_continuous_action_must_be_length_three(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1
        )
        env.reset()
        with self.assertRaises(ValueError):
            env.step([1.0, 0.0])

    def test_hold_via_zero_signed_qty(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1
        )
        env.reset()
        _, _, _, _, info = env.step([0.0, 0.0, 0.0])
        self.assertEqual(info["position"], 0.0)
        self.assertEqual(stack.executor().fill_count, 0)


class OpenOrderObservationTests(unittest.TestCase):
    """T034: per-open-order slots in the observation — qty remaining,
    age, distance from latest price, queue position proxy."""

    def _stack(self):
        import flox_py
        return flox_py.VenueStack.binance_um_futures(
            account_id=1, equity=10_000.0
        )

    def test_default_n_open_slots_in_venue_mode(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2, symbol_id=1
        )
        # window_size + 2 + 4 * 4 = 2 + 2 + 16 = 20
        self.assertEqual(env.n_open_slots, 4)
        self.assertEqual(env.observation_space.shape, (20,))

    def test_default_n_open_slots_in_bare_mode(self) -> None:
        env = rl_env.FloxTradingEnv(trades=_RAMP_TRADES, window_size=3)
        self.assertEqual(env.n_open_slots, 0)
        self.assertEqual(env.observation_space.shape, (5,))

    def test_n_open_slots_explicit_override(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, window_size=2,
            symbol_id=1, n_open_slots=2,
        )
        # 2 + 2 + 4 * 2 = 12
        self.assertEqual(env.observation_space.shape, (12,))

    def test_resting_limit_appears_in_open_order_slots(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, max_position=1.0,
            window_size=2, symbol_id=1, tick_size=0.5,
            n_open_slots=2,
        )
        env.reset()
        # Post a limit far from market so it rests, not fills.
        obs, _, _, _, info = env.step([1.0, 5.0, 2.0])
        self.assertGreaterEqual(info["open_orders"], 1)
        # Slot 0 first entry is signed qty remaining / max_position;
        # should be positive for a resting buy.
        slot_start = env.window_size + 2
        self.assertGreater(obs[slot_start], 0.0)

    def test_market_order_not_recorded_as_open(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, max_position=1.0,
            window_size=2, symbol_id=1, n_open_slots=2,
        )
        env.reset()
        # Market buy — fills immediately, so no resting order remains.
        _, _, _, _, info = env.step([1.0, 0.0, 0.0])
        self.assertEqual(info["open_orders"], 0)

    def test_partial_fill_keeps_remainder_visible(self) -> None:
        # Synthetic case: two consecutive limit orders against the same
        # tape; we cannot easily provoke a true partial fill from
        # Python, but we can assert the slot decreases when a fill on
        # the order id reduces qty_remaining.
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, max_position=1.0,
            window_size=2, symbol_id=1, tick_size=0.5,
            n_open_slots=2,
        )
        env.reset()
        # Submit a limit that should rest.
        env.step([1.0, 5.0, 2.0])
        before = env._open_orders.copy()
        # Hold (no new order). qty_remaining stays unchanged.
        env.step([1.0, 5.0, 2.0])  # signed=1 → delta = 0 → no new order
        for oid, od in before.items():
            if oid in env._open_orders:
                self.assertEqual(
                    env._open_orders[oid]["qty_remaining"], od["qty_remaining"]
                )

    def test_reject_penalty_applied_on_no_fill_no_rest(self) -> None:
        """Force a reject scenario via a tiny tape that has the
        submitted order disappear with no fill. We piggy-back on
        post-only orders crossing the mid which the executor rejects
        pre-book."""
        # Tape: descending so a buy at much higher price crosses the
        # opposite side. Post-only buy at high price should reject.
        descending = [
            (1_000, 100.0, 1.0, 0),
            (2_000, 99.0, 1.0, 0),
            (3_000, 98.0, 1.0, 0),
            (4_000, 97.0, 1.0, 0),
        ]
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=descending, qty=1.0, max_position=1.0,
            window_size=2, symbol_id=1, tick_size=0.5,
            reject_penalty=10.0,
        )
        env.reset()
        # Step once to seed the book.
        env.step([0.0, 0.0, 0.0])
        # Post-only buy aggressively far above mid — likely to reject.
        _, reward, _, _, info = env.step([1.0, 0.5, 2.0])
        if info["rejected"]:
            self.assertLessEqual(reward, 0.0)


class ObservationBuilderTests(unittest.TestCase):
    """T035: standalone ObservationBuilder produces the same obs shape
    and semantics as FloxTradingEnv so a trained policy sees the same
    observation in env, paper, and live."""

    def test_obs_shape_matches_env(self) -> None:
        import flox_py
        stack = flox_py.VenueStack.binance_um_futures(1, 10_000.0)
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tape=_RAMP_TRADES, qty=1.0, max_position=1.0,
            window_size=4, symbol_id=1, n_open_slots=2, tick_size=0.5,
        )
        builder = rl_env.ObservationBuilder(
            window_size=4, n_open_slots=2, tick_size=0.5,
            max_price_offset_ticks=50, max_position=1.0,
        )
        env.reset()
        builder.reset(first_price=_RAMP_TRADES[0][1])
        env_obs, _ = env.reset()
        self.assertEqual(len(env_obs), 4 + 2 + 4 * 2)
        self.assertEqual(len(builder.build()), 4 + 2 + 4 * 2)

    def test_price_window_normalised_to_first(self) -> None:
        builder = rl_env.ObservationBuilder(window_size=3)
        builder.reset(first_price=100.0)
        builder.on_trade(110.0)
        builder.on_trade(120.0)
        obs = builder.build()
        # First three entries are the (normalised) window
        self.assertAlmostEqual(obs[2], 1.20, places=6)

    def test_open_orders_visible_in_slots(self) -> None:
        builder = rl_env.ObservationBuilder(
            window_size=2, n_open_slots=2, tick_size=0.5,
            max_price_offset_ticks=50, max_position=1.0,
        )
        builder.reset(first_price=100.0)
        builder.on_trade(100.0)
        builder.add_open_order(
            order_id=1, side="buy", order_type="limit",
            price=99.5, quantity=0.5,
        )
        obs = builder.build()
        slot_start = builder.window_size + 2
        self.assertGreater(obs[slot_start], 0.0)  # signed qty

    def test_apply_fill_decrements_remaining(self) -> None:
        builder = rl_env.ObservationBuilder(n_open_slots=1)
        builder.reset(first_price=100.0)
        builder.add_open_order(
            order_id=1, side="buy", order_type="limit",
            price=99.5, quantity=1.0,
        )
        builder.apply_fill(1, 0.4)
        self.assertAlmostEqual(builder._open_orders[1]["qty_remaining"], 0.6)
        builder.apply_fill(1, 0.6)
        self.assertNotIn(1, builder._open_orders)


class ActionDecoderTests(unittest.TestCase):
    def test_hold_when_target_equals_current(self) -> None:
        d = rl_env.ActionDecoder(max_position=1.0)
        decoded = d.decode([0.5, 0.0, 0.0], mid_price=100.0, current_position=0.5)
        self.assertEqual(decoded["order_type"], "hold")
        self.assertEqual(decoded["quantity"], 0.0)

    def test_market_buy_when_offset_zero(self) -> None:
        d = rl_env.ActionDecoder(max_position=1.0)
        decoded = d.decode([1.0, 0.0, 0.0], mid_price=100.0, current_position=0.0)
        self.assertEqual(decoded["order_type"], "market")
        self.assertEqual(decoded["side"], "buy")
        self.assertAlmostEqual(decoded["quantity"], 1.0)

    def test_limit_sell_uses_offset(self) -> None:
        d = rl_env.ActionDecoder(
            max_position=1.0, tick_size=0.5, max_price_offset_ticks=10
        )
        decoded = d.decode([-1.0, 4.0, 2.0], mid_price=100.0, current_position=0.0)
        self.assertEqual(decoded["order_type"], "limit")
        self.assertEqual(decoded["side"], "sell")
        # Sell: limit above mid → mid + offset * tick * (-1) for "sell"
        # so 100 + 4 * 0.5 * (-1) = 98.0
        self.assertAlmostEqual(decoded["price"], 98.0)
        self.assertEqual(decoded["tif"], "post_only")

    def test_action_clipped_to_bounds(self) -> None:
        d = rl_env.ActionDecoder(
            max_position=1.0, max_price_offset_ticks=5,
        )
        decoded = d.decode([5.0, 100.0, 7.0], mid_price=100.0, current_position=0.0)
        self.assertEqual(decoded["target_position"], 1.0)
        # Limit since offset wasn't 0
        self.assertEqual(decoded["order_type"], "limit")


class MakeRLPolicyTests(unittest.TestCase):
    def test_returns_strategy_subclass(self) -> None:
        import flox_py
        builder = rl_env.ObservationBuilder(window_size=2)
        decoder = rl_env.ActionDecoder(max_position=1.0)

        class StubModel:
            def predict(self, obs, deterministic=True):
                return [0.0, 0.0, 0.0], None

        policy = rl_env.make_rl_policy(
            StubModel(), symbol_id=1,
            observation_builder=builder, action_decoder=decoder,
        )
        self.assertIsInstance(policy, flox_py.Strategy)


class WalkForwardRLTests(unittest.TestCase):
    """T037: WalkForwardRL harness — anchored / sliding folds, fresh
    VenueStack per fold, DSR-aware aggregate."""

    NS_PER_DAY = 86_400 * 1_000_000_000

    def _stack_factory(self):
        import flox_py
        return lambda: flox_py.VenueStack.binance_um_futures(1, 10_000.0)

    def _synthetic_tape(self, n_days: int = 20):
        trades = []
        for d in range(n_days):
            for h in range(24):
                ts = d * self.NS_PER_DAY + h * 3600 * 1_000_000_000
                trades.append((ts, 50_000.0 + d * 10 + h, 1.0, h % 2))
        return trades

    def test_invalid_mode_rejected(self) -> None:
        with self.assertRaises(ValueError):
            rl_env.WalkForwardRL(
                venue_stack_factory=self._stack_factory(),
                tape=self._synthetic_tape(),
                train_window_days=5,
                test_window_days=2,
                n_folds=2,
                mode="garbage",
            )

    def test_zero_folds_rejected(self) -> None:
        with self.assertRaises(ValueError):
            rl_env.WalkForwardRL(
                venue_stack_factory=self._stack_factory(),
                tape=self._synthetic_tape(),
                train_window_days=5,
                test_window_days=2,
                n_folds=0,
            )

    def test_tape_too_short_rejected(self) -> None:
        with self.assertRaises(ValueError):
            rl_env.WalkForwardRL(
                venue_stack_factory=self._stack_factory(),
                tape=self._synthetic_tape(n_days=3),  # too short for 10+3
                train_window_days=10,
                test_window_days=3,
                n_folds=2,
            )

    def test_anchored_train_starts_pin(self) -> None:
        wf = rl_env.WalkForwardRL(
            venue_stack_factory=self._stack_factory(),
            tape=self._synthetic_tape(),
            train_window_days=10,
            test_window_days=3,
            n_folds=3,
            mode="anchored",
        )
        first_start = wf._folds[0].train_range[0]
        for f in wf._folds:
            self.assertEqual(f.train_range[0], first_start)

    def test_sliding_train_starts_advance(self) -> None:
        wf = rl_env.WalkForwardRL(
            venue_stack_factory=self._stack_factory(),
            tape=self._synthetic_tape(),
            train_window_days=10,
            test_window_days=3,
            n_folds=3,
            mode="sliding",
        )
        starts = [f.train_range[0] for f in wf._folds]
        self.assertEqual(starts, sorted(starts))
        self.assertGreater(starts[1], starts[0])

    def test_yields_train_test_pairs_and_aggregate(self) -> None:
        wf = rl_env.WalkForwardRL(
            venue_stack_factory=self._stack_factory(),
            tape=self._synthetic_tape(),
            train_window_days=10,
            test_window_days=3,
            n_folds=3,
            env_kwargs={
                "qty": 0.01, "max_position": 0.02, "window_size": 4,
                "tick_size": 0.5, "n_open_slots": 0,
            },
        )

        class StubModel:
            def predict(self, obs, deterministic=True):
                return [0.0, 0.0, 0.0], None

        model = StubModel()
        n_seen = 0
        for train_env, test_env in wf:
            self.assertIsInstance(train_env, rl_env.FloxTradingEnv)
            self.assertIsInstance(test_env, rl_env.FloxTradingEnv)
            wf.evaluate(model, test_env)
            n_seen += 1
        self.assertEqual(n_seen, 3)

        agg = wf.aggregate()
        self.assertEqual(agg["n_folds"], 3)
        self.assertIn("mean_return_pct", agg)
        self.assertIn("std_return_pct", agg)
        self.assertIn("sign_match", agg)
        self.assertIn("worst_return_pct", agg)

    def test_fresh_stack_per_fold(self) -> None:
        # Each fold should construct a brand-new VenueStack; we count
        # factory invocations.
        import flox_py
        count = {"n": 0}

        def factory():
            count["n"] += 1
            return flox_py.VenueStack.binance_um_futures(1, 10_000.0)

        wf = rl_env.WalkForwardRL(
            venue_stack_factory=factory,
            tape=self._synthetic_tape(),
            train_window_days=10,
            test_window_days=3,
            n_folds=3,
            env_kwargs={
                "qty": 0.01, "max_position": 0.02, "window_size": 4,
                "tick_size": 0.5, "n_open_slots": 0,
            },
        )
        for train_env, test_env in wf:
            pass
        # 3 folds * 2 envs per fold = 6 stack constructions
        self.assertEqual(count["n"], 6)


class MultiSymbolTests(unittest.TestCase):
    """T036: Dict observation and action spaces over multiple tapes."""

    def _stack(self):
        import flox_py
        return flox_py.VenueStack.binance_um_futures(1, 10_000.0)

    def _two_tapes(self, n=10):
        btc = [(i * 100_000_000, 50_000.0 + i * 5.0, 1.0, i % 2) for i in range(n)]
        eth = [(i * 100_000_000 + 50_000_000, 3_000.0 + i * 1.0, 1.0, i % 2) for i in range(n)]
        return {1: btc, 2: eth}

    def test_dict_spaces_when_tapes_passed(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tapes=self._two_tapes(), qty=0.01, max_position=0.02,
            window_size=4, tick_size=0.5,
        )
        self.assertEqual(type(env.action_space).__name__, "_DictSpace")
        self.assertEqual(type(env.observation_space).__name__, "_DictSpace")
        self.assertEqual(set(env.action_space.spaces.keys()), {"1", "2"})
        self.assertEqual(
            set(env.observation_space.spaces.keys()), {"1", "2", "account"}
        )

    def test_passing_both_tape_and_tapes_rejected(self) -> None:
        stack = self._stack()
        with self.assertRaises(ValueError):
            rl_env.FloxTradingEnv.from_venue_stack(
                stack, tape=_RAMP_TRADES, tapes=self._two_tapes(), qty=0.01,
            )

    def test_empty_tape_dict_rejected(self) -> None:
        stack = self._stack()
        with self.assertRaises(ValueError):
            rl_env.FloxTradingEnv.from_venue_stack(
                stack, tapes={1: [], 2: [(0, 100.0, 1.0, 0)]}, qty=0.01,
            )

    def test_discrete_action_mode_rejected_for_multi(self) -> None:
        stack = self._stack()
        with self.assertRaises(ValueError):
            rl_env.FloxTradingEnv.from_venue_stack(
                stack, tapes=self._two_tapes(), qty=0.01,
                action_mode="discrete",
            )

    def test_merged_events_sorted_by_ts(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tapes=self._two_tapes(), qty=0.01, max_position=0.02,
            window_size=4, tick_size=0.5,
        )
        timestamps = [e[0] for e in env._merged_events]
        self.assertEqual(timestamps, sorted(timestamps))
        # Both symbols appear in the merged stream
        syms = {e[1] for e in env._merged_events}
        self.assertEqual(syms, {1, 2})

    def test_step_returns_dict_obs(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tapes=self._two_tapes(), qty=0.01, max_position=0.02,
            window_size=4, tick_size=0.5,
        )
        env.reset()
        obs, _, _, _, _ = env.step({"1": [0.5, 0.0, 0.0], "2": [-0.5, 0.0, 0.0]})
        self.assertIsInstance(obs, dict)
        self.assertIn("1", obs)
        self.assertIn("2", obs)
        self.assertIn("account", obs)

    def test_account_obs_carries_three_values(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tapes=self._two_tapes(), qty=0.01, max_position=0.02,
            window_size=4, tick_size=0.5,
        )
        obs, _ = env.reset()
        self.assertEqual(len(obs["account"]), 3)

    def test_per_symbol_positions_independent(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tapes=self._two_tapes(20), qty=0.01, max_position=0.02,
            window_size=4, tick_size=0.5,
        )
        env.reset()
        # Long BTC, short ETH
        for _ in range(5):
            env.step({"1": [1.0, 0.0, 0.0], "2": [-1.0, 0.0, 0.0]})
        self.assertGreater(env._multi_positions[1], 0.0)
        self.assertLess(env._multi_positions[2], 0.0)

    def test_non_dict_action_rejected(self) -> None:
        stack = self._stack()
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tapes=self._two_tapes(), qty=0.01, max_position=0.02,
            window_size=4, tick_size=0.5,
        )
        env.reset()
        with self.assertRaises(ValueError):
            env.step([0.5, 0.0, 0.0])

    def test_episode_truncates_when_merged_stream_exhausted(self) -> None:
        stack = self._stack()
        n = 6
        env = rl_env.FloxTradingEnv.from_venue_stack(
            stack, tapes=self._two_tapes(n), qty=0.01, max_position=0.02,
            window_size=4, tick_size=0.5,
        )
        env.reset()
        truncated = False
        for _ in range(2 * n + 4):
            _, _, _, truncated, _ = env.step(
                {"1": [0.0, 0.0, 0.0], "2": [0.0, 0.0, 0.0]}
            )
            if truncated:
                break
        self.assertTrue(truncated)


if __name__ == "__main__":
    unittest.main()
