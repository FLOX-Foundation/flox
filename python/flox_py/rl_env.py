"""Gymnasium-compatible RL environment over a flox tape.

The environment drives a ``flox_py.SimulatedExecutor`` through the
trades in a captured tape and exposes the Gymnasium ``Env`` API
that ``stable_baselines3``, ``RLlib``, and ``CleanRL`` already
speak. The default action space is discrete (hold / buy / sell)
and the observation vector packs the recent price window, current
position, and unrealized PnL; both are configurable.

This module does not import ``gymnasium``. The class implements the
``Env`` protocol structurally so callers can plug it into any RL
library without flox dragging gymnasium into its dependency surface.
Pass a constructed ``FloxTradingEnv`` straight into
``gymnasium.make`` or to a learner; the duck-typed ``observation_space``
and ``action_space`` mirror the ``gymnasium.spaces`` API closely
enough for that to work.

Two construction paths:

- ``FloxTradingEnv.from_tape(...)`` — trade-by-trade replay with
  ideal fills (no fees, no funding, no liquidation, no queue
  position). Useful for prototyping and for testing the agent
  pipeline. Numbers it produces are optimistic.
- ``FloxTradingEnv.from_venue_stack(stack, tape, ...)`` — routes
  every action through ``stack.executor()`` so fills, fees,
  funding, liquidation, rate limits, and venue availability are
  the same simulated subsystems used in backtest, paper trading,
  and the broker pattern. Reward is the change in account equity,
  with fees deducted on each fill. Episodes terminate on
  account-level liquidation.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Deque, List, Optional, Sequence, Tuple


# ── Minimal space implementations (duck-typed against gymnasium) ──


@dataclass
class _DiscreteSpace:
    n: int
    dtype: str = "int64"

    def sample(self) -> int:
        import random as _random
        return _random.randrange(self.n)

    def contains(self, x: Any) -> bool:
        return isinstance(x, int) and 0 <= x < self.n


@dataclass
class _BoxSpace:
    low: Sequence[float]
    high: Sequence[float]
    shape: Tuple[int, ...]
    dtype: str = "float32"

    def sample(self) -> List[float]:
        import random as _random
        return [
            _random.uniform(float(lo), float(hi))
            for lo, hi in zip(self.low, self.high)
        ]

    def contains(self, x: Sequence[float]) -> bool:
        try:
            if len(x) != self.shape[0]:
                return False
        except TypeError:
            return False
        return all(
            float(lo) - 1e-6 <= float(v) <= float(hi) + 1e-6
            for v, lo, hi in zip(x, self.low, self.high)
        )


# ── Trade source ──────────────────────────────────────────────────


def _load_tape_trades(path: str | Path) -> List[Tuple[int, float, float, int]]:
    """Read every trade from a ``.floxlog`` tape into a list of
    ``(ts_ns, price, qty, side)`` tuples. Loaded once at construction;
    episode playback then iterates the list."""
    import flox_py
    import numpy as np  # noqa: F401  — reader returns a structured ndarray

    reader = flox_py.DataReader(str(path))
    trades = reader.read_trades()
    out: List[Tuple[int, float, float, int]] = []
    for row in trades:
        out.append((
            int(row["exchange_ts_ns"]),
            float(row["price_raw"]) / 1e8,
            float(row["qty_raw"]) / 1e8,
            int(row["side"]),
        ))
    return out


# ── Environment ───────────────────────────────────────────────────


@dataclass
class FloxTradingEnv:
    """Gymnasium-compatible environment over a flox tape.

    Action space (discrete, default):

    * 0 — hold (no order)
    * 1 — go long ``qty`` if flat or short
    * 2 — go short ``qty`` if flat or long

    Observation (default ``Box`` of length ``window_size + 2``):

    * The last ``window_size`` mid-prices, normalized by the first
      observed price
    * Current position quantity (signed)
    * Unrealized PnL since the last position change

    Reward in the bare path is the PnL delta (realized + unrealized)
    since the previous step. In the venue-stack path it is the
    change in account equity (which already accounts for fees and
    funding). Override ``reward_fn`` to customize either.
    """

    trades: Sequence[Tuple[int, float, float, int]] = field(default_factory=list)
    qty: float = 1.0
    window_size: int = 8
    reward_fn: Optional[Callable[["FloxTradingEnv", Any], float]] = None
    seed: Optional[int] = None

    # Venue-stack mode: when set, step() routes orders through the
    # stack's simulated executor and uses the account's equity for
    # the reward signal. None → Phase-1 bare-fill mode.
    venue_stack: Optional[Any] = None
    symbol_id: int = 1
    # Fee side assumed when recording fills for tier tracking and fee
    # deduction. Market orders are takers; limit orders sitting on the
    # book are makers. The discrete-action Phase 1 surface only emits
    # market orders, so this defaults to taker.
    is_maker: bool = False

    # Filled at __post_init__.
    action_space: _DiscreteSpace = field(init=False)
    observation_space: _BoxSpace = field(init=False)

    # Episode state.
    _idx: int = field(default=0, init=False)
    _position: float = field(default=0.0, init=False)
    _entry_price: float = field(default=0.0, init=False)
    _realized_pnl: float = field(default=0.0, init=False)
    _last_total_pnl: float = field(default=0.0, init=False)
    _price_window: Deque[float] = field(default_factory=deque, init=False)
    _first_price: float = field(default=0.0, init=False)

    # Venue-stack mode internal state.
    _next_order_id: int = field(default=0, init=False)
    _last_fill_idx: int = field(default=0, init=False)
    _last_equity_at_mark: float = field(default=0.0, init=False)
    _venue_entry_price: float = field(default=0.0, init=False)
    _last_outcome: dict = field(default_factory=dict, init=False)

    # Static metadata gymnasium learners look at.
    metadata: dict = field(default_factory=lambda: {"render_modes": []})

    def __post_init__(self) -> None:
        if not self.trades:
            raise ValueError(
                "FloxTradingEnv needs a non-empty trade list; "
                "construct with from_tape() or pass `trades=...`"
            )
        if self.qty <= 0:
            raise ValueError(f"qty must be positive; got {self.qty}")
        if self.window_size <= 0:
            raise ValueError(f"window_size must be positive; got {self.window_size}")

        self.action_space = _DiscreteSpace(n=3)
        # Observation bounds: prices are positive but unbounded;
        # position is bounded by ±qty (Phase 1 single-step long/short
        # only); PnL is unbounded.
        obs_dim = self.window_size + 2
        self.observation_space = _BoxSpace(
            low=[0.0] * self.window_size + [-self.qty, -1e9],
            high=[1e9] * self.window_size + [self.qty, 1e9],
            shape=(obs_dim,),
            dtype="float32",
        )

    # ── Constructors ─────────────────────────────────────────────

    @classmethod
    def from_tape(
        cls,
        path: str | Path,
        *,
        qty: float = 1.0,
        window_size: int = 8,
        reward_fn: Optional[Callable[["FloxTradingEnv", Any], float]] = None,
        seed: Optional[int] = None,
    ) -> "FloxTradingEnv":
        """Convenience constructor that loads every trade from a
        ``.floxlog`` tape directory. Ideal-fill path: no fees,
        funding, liquidation, or queue position. Use this for
        prototyping; switch to ``from_venue_stack`` for any
        backtest you would put real capital behind."""
        return cls(
            trades=_load_tape_trades(path),
            qty=qty,
            window_size=window_size,
            reward_fn=reward_fn,
            seed=seed,
        )

    @classmethod
    def from_venue_stack(
        cls,
        venue_stack: Any,
        *,
        tape: str | Path | Sequence[Tuple[int, float, float, int]],
        qty: float = 1.0,
        window_size: int = 8,
        symbol_id: int = 1,
        is_maker: bool = False,
        reward_fn: Optional[Callable[["FloxTradingEnv", Any], float]] = None,
        seed: Optional[int] = None,
    ) -> "FloxTradingEnv":
        """Construct an env backed by a ``VenueStack``.

        Every order the agent submits goes through ``stack.executor()``,
        so fills, fees, funding, liquidation walks, rate-limit policy,
        and venue availability are the same simulated subsystems the
        rest of the W15 stack uses. Reward is the change in account
        equity at mark, with taker (or maker, if ``is_maker=True``)
        fees deducted on each fill via the stack's fee schedule.

        Episodes terminate on the first liquidation event reported by
        ``stack.liquidation()``.

        ``tape`` can be a path to a .floxlog directory or an
        in-memory sequence of ``(ts_ns, price, qty, side)`` tuples.
        """
        trades = (
            _load_tape_trades(tape)
            if isinstance(tape, (str, Path))
            else list(tape)
        )
        return cls(
            trades=trades,
            qty=qty,
            window_size=window_size,
            reward_fn=reward_fn,
            seed=seed,
            venue_stack=venue_stack,
            symbol_id=symbol_id,
            is_maker=is_maker,
        )

    # ── Gymnasium API ────────────────────────────────────────────

    def reset(
        self, *, seed: Optional[int] = None, options: Optional[dict] = None
    ) -> Tuple[List[float], dict]:
        if seed is not None:
            self.seed = seed
        self._idx = 0
        self._position = 0.0
        self._entry_price = 0.0
        self._realized_pnl = 0.0
        self._last_total_pnl = 0.0
        self._next_order_id = 0
        self._last_fill_idx = 0
        self._venue_entry_price = 0.0
        self._last_outcome = {}
        self._price_window.clear()
        first_price = float(self.trades[0][1])
        self._first_price = first_price
        for _ in range(self.window_size):
            self._price_window.append(first_price)

        if self.venue_stack is not None:
            # Equity-at-mark baseline so the first step's reward is
            # the change since reset, not since some implicit zero.
            acct = self.venue_stack.account()
            self._last_equity_at_mark = float(
                acct.equity() + acct.total_unrealised_pnl()
            )

        obs = self._observation()
        info = {"step": self._idx, "position": self._position}
        return obs, info

    def step(self, action: int) -> Tuple[List[float], float, bool, bool, dict]:
        if not self.action_space.contains(int(action)):
            raise ValueError(
                f"action {action!r} not in action_space (n=3)"
            )

        if self.venue_stack is not None:
            return self._step_venue_stack(int(action))
        return self._step_bare(int(action))

    def render(self) -> None:
        # Render mode is intentionally a no-op for Phase 1; the
        # tooling that visualizes trades belongs to the replay viewer.
        pass

    def close(self) -> None:
        # Nothing to close; the trade list is plain Python data.
        pass

    # ── Internal — bare-fill (Phase 1) path ──────────────────────

    def _step_bare(self, action: int) -> Tuple[List[float], float, bool, bool, dict]:
        ts_ns, price, _, _ = self.trades[self._idx]
        self._price_window.append(float(price))
        if len(self._price_window) > self.window_size:
            self._price_window.popleft()

        target_position = {
            0: self._position,
            1: float(self.qty),
            2: -float(self.qty),
        }[action]
        if target_position != self._position:
            if self._position != 0.0:
                pnl_close = (price - self._entry_price) * self._position
                self._realized_pnl += pnl_close
            self._position = target_position
            self._entry_price = (
                float(price) if target_position != 0.0 else 0.0
            )

        unrealized = (
            (price - self._entry_price) * self._position
            if self._position != 0.0
            else 0.0
        )
        total_pnl = self._realized_pnl + unrealized
        reward_default = total_pnl - self._last_total_pnl
        self._last_total_pnl = total_pnl

        if self.reward_fn is not None:
            ctx = {
                "ts_ns": ts_ns,
                "price": price,
                "position": self._position,
                "realized_pnl": self._realized_pnl,
                "unrealized_pnl": unrealized,
                "step": self._idx,
            }
            reward = float(self.reward_fn(self, ctx))
        else:
            reward = float(reward_default)

        self._idx += 1
        terminated = False
        truncated = self._idx >= len(self.trades)

        info = {
            "ts_ns": ts_ns,
            "price": price,
            "position": self._position,
            "realized_pnl": self._realized_pnl,
            "unrealized_pnl": unrealized,
            "total_pnl": total_pnl,
            "step": self._idx,
        }
        return self._observation(), reward, terminated, truncated, info

    # ── Internal — venue-stack path ─────────────────────────────

    def _step_venue_stack(
        self, action: int
    ) -> Tuple[List[float], float, bool, bool, dict]:
        stack = self.venue_stack
        exec_ = stack.executor()
        acct = stack.account()
        liq = stack.liquidation()
        fees = stack.fees()

        ts_ns, price, trade_qty, trade_side = self.trades[self._idx]
        self._price_window.append(float(price))
        if len(self._price_window) > self.window_size:
            self._price_window.popleft()

        # Translate action → order intent. The bare path uses a fixed
        # target (long qty / short qty / hold); the venue path mirrors
        # that, submitting market orders for the delta between current
        # and target position.
        target_position = {
            0: self._position,
            1: float(self.qty),
            2: -float(self.qty),
        }[action]
        delta = target_position - self._position
        if delta != 0.0:
            side = "buy" if delta > 0 else "sell"
            self._next_order_id += 1
            exec_.submit_order(
                id=self._next_order_id,
                side=side,
                price=float(price),
                quantity=abs(delta),
                type="market",
                symbol=self.symbol_id,
                tif="gtc",
                account_id=acct.account_id(),
            )

        # Feed the current tick to the matching engine so any pending
        # market or limit order can fill.
        exec_.on_trade_qty(
            self.symbol_id, float(price), float(trade_qty), trade_side == 0
        )

        # Drain fills produced during this step.
        all_fills = exec_.fills_list()
        new_fills = all_fills[self._last_fill_idx :]
        self._last_fill_idx = len(all_fills)
        for f in new_fills:
            fill_qty = float(f["quantity"])
            fill_price = float(f["price"])
            signed = fill_qty if f["side"] == "buy" else -fill_qty
            self._apply_fill(acct, signed, fill_price)

            # Record notional for tier tracking and deduct the fee from
            # account equity. record_fill bumps the 30d aggregate
            # FeeSchedule uses; fee_for asks the schedule what the
            # actual fee is at the current tier.
            notional = fill_price * fill_qty
            fees.record_fill(int(ts_ns), notional)
            fee = float(fees.fee_for(int(ts_ns), notional, self.is_maker))
            acct.add_equity(-fee)

        # Mark + liquidation walk.
        acct.set_mark(self.symbol_id, float(price), int(ts_ns))
        outcome = liq.on_mark(self.symbol_id, float(price))
        self._last_outcome = outcome
        terminated = bool(outcome.get("liquidations_count", 0))

        # Reward = change in equity-at-mark since previous step. This
        # naturally folds in realized PnL, unrealized PnL, fees just
        # deducted, and funding the schedule applied (if any).
        equity_now = float(acct.equity() + acct.total_unrealised_pnl())
        reward_default = equity_now - self._last_equity_at_mark
        self._last_equity_at_mark = equity_now

        if self.reward_fn is not None:
            ctx = {
                "ts_ns": ts_ns,
                "price": price,
                "position": self._position,
                "equity": acct.equity(),
                "unrealized_pnl": acct.total_unrealised_pnl(),
                "equity_at_mark": equity_now,
                "liquidation_outcome": outcome,
                "step": self._idx,
            }
            reward = float(self.reward_fn(self, ctx))
        else:
            reward = float(reward_default)

        self._idx += 1
        truncated = self._idx >= len(self.trades)

        info = {
            "ts_ns": ts_ns,
            "price": price,
            "position": self._position,
            "equity": acct.equity(),
            "unrealized_pnl": acct.total_unrealised_pnl(),
            "equity_at_mark": equity_now,
            "fee_tier": fees.current_tier_index(),
            "liquidation_outcome": outcome,
            "step": self._idx,
        }
        return self._observation(), reward, terminated, truncated, info

    def _apply_fill(self, acct: Any, signed: float, fill_price: float) -> None:
        """Update position, weighted entry, and realize PnL into account
        equity. The bound Account exposes open_position / close_position
        but does not realize PnL into equity on close; we model that
        explicitly here via add_equity. Same-direction adds keep a
        weighted entry; flips realize the closed portion first then
        re-open at the fill price."""
        old_pos = self._position
        new_pos = old_pos + signed
        sym = self.symbol_id

        if abs(old_pos) < 1e-12:
            if abs(new_pos) > 1e-12:
                acct.open_position(sym, new_pos, fill_price)
                self._venue_entry_price = fill_price
            else:
                self._venue_entry_price = 0.0
        elif old_pos * signed > 0:
            old_notional = self._venue_entry_price * old_pos
            new_notional = fill_price * signed
            self._venue_entry_price = (old_notional + new_notional) / new_pos
            acct.close_position(sym)
            acct.open_position(sym, new_pos, self._venue_entry_price)
        elif old_pos * new_pos >= 0:
            closed_qty = abs(signed)
            direction = 1.0 if old_pos > 0 else -1.0
            realized = (fill_price - self._venue_entry_price) * direction * closed_qty
            acct.add_equity(realized)
            acct.close_position(sym)
            if abs(new_pos) > 1e-12:
                acct.open_position(sym, new_pos, self._venue_entry_price)
            else:
                self._venue_entry_price = 0.0
        else:
            closed_qty = abs(old_pos)
            direction = 1.0 if old_pos > 0 else -1.0
            realized = (fill_price - self._venue_entry_price) * direction * closed_qty
            acct.add_equity(realized)
            acct.close_position(sym)
            acct.open_position(sym, new_pos, fill_price)
            self._venue_entry_price = fill_price

        self._position = new_pos

    # ── Internal — shared observation ────────────────────────────

    def _observation(self) -> List[float]:
        denom = self._first_price if self._first_price > 0 else 1.0
        normalized = [p / denom for p in self._price_window]
        unreal = 0.0
        if self._position != 0.0 and self._idx < len(self.trades):
            cur_price = float(self.trades[self._idx][1])
            unreal = (cur_price - self._entry_price) * self._position
        return list(normalized) + [self._position, unreal]


__all__ = [
    "FloxTradingEnv",
]
