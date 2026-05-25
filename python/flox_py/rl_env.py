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

    # Action mode: "discrete" → Discrete(3) (hold / long qty / short qty,
    # Phase 1 default). "continuous" → Box((3,)) (signed qty fraction,
    # price offset in ticks, TIF flag). from_venue_stack picks
    # "continuous" by default; from_tape stays "discrete".
    action_mode: str = "discrete"
    max_position: float = 1.0
    tick_size: float = 0.01
    max_price_offset_ticks: int = 50

    # Filled at __post_init__.
    action_space: Any = field(init=False)
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

        if self.action_mode not in ("discrete", "continuous"):
            raise ValueError(
                f"action_mode must be 'discrete' or 'continuous'; "
                f"got {self.action_mode!r}"
            )
        if self.action_mode == "discrete":
            self.action_space = _DiscreteSpace(n=3)
        else:
            # Axes:
            #   0 — signed quantity as a fraction of max_position [-1, 1]
            #   1 — price offset from mid in ticks; 0 means market
            #   2 — TIF flag, rounded to int: 0=GTC, 1=IOC, 2=Post-only
            offset = float(self.max_price_offset_ticks)
            self.action_space = _BoxSpace(
                low=[-1.0, -offset, 0.0],
                high=[1.0, offset, 2.0],
                shape=(3,),
                dtype="float32",
            )

        # Observation bounds: prices are positive but unbounded;
        # position is bounded by ±max_position; PnL is unbounded.
        obs_dim = self.window_size + 2
        pos_bound = max(float(self.qty), float(self.max_position))
        self.observation_space = _BoxSpace(
            low=[0.0] * self.window_size + [-pos_bound, -1e9],
            high=[1e9] * self.window_size + [pos_bound, 1e9],
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
        action_mode: str = "continuous",
        max_position: Optional[float] = None,
        tick_size: float = 0.01,
        max_price_offset_ticks: int = 50,
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
            action_mode=action_mode,
            max_position=float(max_position if max_position is not None else qty),
            tick_size=tick_size,
            max_price_offset_ticks=max_price_offset_ticks,
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

    def step(self, action: Any) -> Tuple[List[float], float, bool, bool, dict]:
        if self.action_mode == "discrete":
            try:
                action_int = int(action)
            except (TypeError, ValueError) as exc:
                raise ValueError(
                    f"action {action!r} is not a valid discrete action"
                ) from exc
            if not self.action_space.contains(action_int):
                raise ValueError(
                    f"action {action!r} not in action_space (n=3)"
                )
            if self.venue_stack is not None:
                return self._step_venue_stack_discrete(action_int)
            return self._step_bare(action_int)

        # Continuous mode — Box((3,)). Clip out-of-bounds rather than
        # reject so learners that occasionally sample outside the range
        # do not crash the env; the clip is recorded in info.
        seq = self._coerce_action(action)
        clipped, was_clipped = self._clip_continuous(seq)
        if self.venue_stack is None:
            return self._step_bare_continuous(clipped, was_clipped)
        return self._step_venue_stack_continuous(clipped, was_clipped)

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

    def _step_venue_stack_discrete(
        self, action: int
    ) -> Tuple[List[float], float, bool, bool, dict]:
        target_position = {
            0: self._position,
            1: float(self.qty),
            2: -float(self.qty),
        }[action]
        delta = target_position - self._position
        return self._step_venue_stack_common(
            delta_qty=delta,
            order_type="market",
            limit_price=None,
            tif="gtc",
            was_clipped=False,
        )

    def _step_venue_stack_continuous(
        self, clipped: Tuple[float, float, float], was_clipped: bool
    ) -> Tuple[List[float], float, bool, bool, dict]:
        signed_frac, offset_ticks, tif_axis = clipped
        target_position = float(signed_frac) * float(self.max_position)
        delta = target_position - self._position
        offset_int = int(round(offset_ticks))
        tif_int = int(round(tif_axis))
        tif_name = {0: "gtc", 1: "ioc", 2: "post_only"}.get(tif_int, "gtc")
        if offset_int == 0:
            return self._step_venue_stack_common(
                delta_qty=delta,
                order_type="market",
                limit_price=None,
                tif="gtc",  # market orders ignore TIF in practice
                was_clipped=was_clipped,
            )
        # Limit order at mid ± offset_ticks * tick_size. Mid is
        # approximated by the most recent trade price; once the env
        # tracks best bid / best ask explicitly (T034) this becomes
        # exact.
        mid = float(self.trades[self._idx][1])
        side_sign = 1.0 if delta > 0 else -1.0
        limit_price = mid + offset_int * float(self.tick_size) * side_sign
        return self._step_venue_stack_common(
            delta_qty=delta,
            order_type="limit",
            limit_price=limit_price,
            tif=tif_name,
            was_clipped=was_clipped,
        )

    def _step_venue_stack_common(
        self,
        *,
        delta_qty: float,
        order_type: str,
        limit_price: Optional[float],
        tif: str,
        was_clipped: bool,
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

        if abs(delta_qty) > 1e-12:
            side = "buy" if delta_qty > 0 else "sell"
            self._next_order_id += 1
            exec_.submit_order(
                id=self._next_order_id,
                side=side,
                price=float(limit_price)
                if limit_price is not None
                else float(price),
                quantity=abs(delta_qty),
                type=order_type,
                symbol=self.symbol_id,
                tif=tif,
                account_id=acct.account_id(),
            )

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
            "action_clipped": was_clipped,
            "order_type": order_type,
            "step": self._idx,
        }
        return self._observation(), reward, terminated, truncated, info

    def _step_bare_continuous(
        self, clipped: Tuple[float, float, float], was_clipped: bool
    ) -> Tuple[List[float], float, bool, bool, dict]:
        # The bare path ignores limit semantics — there is no executor
        # to honour them — so continuous mode here collapses to the
        # signed-qty axis interpreted against max_position. price
        # offset and TIF are recorded in info but otherwise ignored.
        signed_frac, _offset_ticks, _tif_axis = clipped
        target_position = float(signed_frac) * float(self.max_position)
        # Reuse the bare path's machinery by mapping the continuous
        # target back onto the discrete {hold, long, short} buckets,
        # since the bare path's _position bookkeeping is fixed-qty.
        if target_position > 1e-12:
            action_int = 1
        elif target_position < -1e-12:
            action_int = 2
        else:
            action_int = 0
        obs, reward, terminated, truncated, info = self._step_bare(action_int)
        info["action_clipped"] = was_clipped
        return obs, reward, terminated, truncated, info

    def _coerce_action(self, action: Any) -> Tuple[float, float, float]:
        """Accept numpy arrays, lists, tuples — anything with three
        float-coercible entries."""
        try:
            seq = list(action)
        except TypeError as exc:
            raise ValueError(
                f"continuous action {action!r} is not iterable"
            ) from exc
        if len(seq) != 3:
            raise ValueError(
                f"continuous action must have length 3; got {len(seq)}"
            )
        return float(seq[0]), float(seq[1]), float(seq[2])

    def _clip_continuous(
        self, action: Tuple[float, float, float]
    ) -> Tuple[Tuple[float, float, float], bool]:
        lo = self.action_space.low
        hi = self.action_space.high
        clipped = (
            min(max(action[0], float(lo[0])), float(hi[0])),
            min(max(action[1], float(lo[1])), float(hi[1])),
            min(max(action[2], float(lo[2])), float(hi[2])),
        )
        was_clipped = any(
            abs(c - a) > 1e-12 for c, a in zip(clipped, action)
        )
        if was_clipped:
            import warnings as _warnings
            _warnings.warn(
                f"continuous action {action} clipped to {clipped}",
                RuntimeWarning,
                stacklevel=3,
            )
        return clipped, was_clipped

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
