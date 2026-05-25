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
from typing import Any, Callable, Deque, Dict, List, Optional, Sequence, Tuple


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
class _DictSpace:
    """Duck-typed `gymnasium.spaces.Dict`. Holds an ordered mapping of
    name → sub-space. Used in multi-symbol mode where the observation
    and action are per-symbol bundles plus an optional account-level
    slot."""

    spaces: Dict[str, Any]

    @property
    def dtype(self) -> str:
        return "dict"

    def sample(self) -> Dict[str, Any]:
        return {k: v.sample() for k, v in self.spaces.items()}

    def contains(self, x: Any) -> bool:
        if not isinstance(x, dict):
            return False
        if set(x.keys()) != set(self.spaces.keys()):
            return False
        return all(self.spaces[k].contains(x[k]) for k in self.spaces)


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

    # Open-order observation slots. Each slot is four floats:
    # [signed_qty_remaining / max_position, age_in_steps / window_size,
    #  distance_from_latest_price_in_ticks / max_price_offset_ticks,
    #  queue_position_proxy in [0, 1]]. Unused slots are zeros.
    # None → default to 4 in venue-stack mode and 0 in the bare path
    # (bare has no resting orders to track).
    n_open_slots: Optional[int] = None
    # Optional negative reward applied when our submit_order call did
    # not produce a fill and there is no resting order — proxy for a
    # rate-limit / post-only / venue-availability reject. Default 0
    # leaves the behaviour unchanged.
    reject_penalty: float = 0.0

    # Multi-symbol mode. When set, the env runs over a merged stream
    # of per-symbol tapes and exposes Dict observation / action spaces
    # keyed by symbol ID. Single-symbol behaviour is unchanged when
    # this is None.
    multi_tapes: Optional[Dict[int, Sequence[Tuple[int, float, float, int]]]] = None

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
    # order_id → dict(side, type, price, qty_remaining, submit_step,
    # initial_qty). Maintained as fills land — orders disappear when
    # qty_remaining ≈ 0 or after explicit cancel.
    _open_orders: Dict[int, dict] = field(default_factory=dict, init=False)

    # Multi-symbol mode internals. Each entry is the per-symbol slice
    # of the state the single-symbol path tracks at the top level.
    _merged_events: List[Tuple[int, int, float, float, int]] = field(
        default_factory=list, init=False
    )  # (ts_ns, symbol_id, price, qty, side) sorted by ts_ns
    _multi_positions: Dict[int, float] = field(default_factory=dict, init=False)
    _multi_entry_prices: Dict[int, float] = field(
        default_factory=dict, init=False
    )
    _multi_open_orders: Dict[int, Dict[int, dict]] = field(
        default_factory=dict, init=False
    )
    _multi_last_fill_idx: int = field(default=0, init=False)

    # Static metadata gymnasium learners look at.
    metadata: dict = field(default_factory=lambda: {"render_modes": []})

    def __post_init__(self) -> None:
        if self.multi_tapes is not None:
            self._init_multi_symbol()
            return

        if not self.trades:
            raise ValueError(
                "FloxTradingEnv needs a non-empty trade list; "
                "construct with from_tape() or pass `trades=...`"
            )
        if self.qty <= 0:
            raise ValueError(f"qty must be positive; got {self.qty}")
        if self.window_size <= 0:
            raise ValueError(f"window_size must be positive; got {self.window_size}")

        if self.n_open_slots is None:
            self.n_open_slots = 4 if self.venue_stack is not None else 0

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
        # position is bounded by ±max_position; PnL is unbounded;
        # open-order slots are normalised so each entry sits in
        # roughly [-1, 1] but we leave headroom for early-step
        # observations.
        slot_count = max(0, int(self.n_open_slots))
        obs_dim = self.window_size + 2 + 4 * slot_count
        pos_bound = max(float(self.qty), float(self.max_position))
        slot_low = [-1.0, 0.0, -1.0, 0.0] * slot_count
        slot_high = [1.0, 10.0, 1.0, 1.0] * slot_count
        self.observation_space = _BoxSpace(
            low=[0.0] * self.window_size + [-pos_bound, -1e9] + slot_low,
            high=[1e9] * self.window_size + [pos_bound, 1e9] + slot_high,
            shape=(obs_dim,),
            dtype="float32",
        )

    def _init_multi_symbol(self) -> None:
        """Multi-symbol mode setup. The agent sees a Dict observation
        keyed by symbol_id (string-coerced) plus an "account" slot;
        the action space is Dict over the same symbols. One env step
        consumes one event from the merged tape stream — at that
        timestamp every symbol's action is decoded and applied. A
        symbol whose target position is unchanged simply does not
        emit an order."""
        if not self.multi_tapes:
            raise ValueError("multi_tapes must be a non-empty mapping")
        if self.qty <= 0:
            raise ValueError(f"qty must be positive; got {self.qty}")
        if self.window_size <= 0:
            raise ValueError(
                f"window_size must be positive; got {self.window_size}"
            )
        if self.venue_stack is None:
            raise ValueError(
                "multi-symbol mode requires a VenueStack; use "
                "FloxTradingEnv.from_venue_stack(stack, tapes={...})"
            )
        if self.n_open_slots is None:
            self.n_open_slots = 2  # smaller default per symbol
        if self.action_mode != "continuous":
            raise ValueError(
                "multi-symbol mode only supports continuous action_mode"
            )

        # Normalize keys to int symbol IDs; strings are also accepted
        # but stored as ints. Action / obs dict keys mirror this — we
        # use str(sym_id) so gymnasium's Dict accepts hashable keys
        # cleanly.
        normalised: Dict[int, List[Tuple[int, float, float, int]]] = {}
        for k, v in self.multi_tapes.items():
            normalised[int(k)] = list(v)
            if not normalised[int(k)]:
                raise ValueError(f"tape for symbol {k!r} is empty")
        self.multi_tapes = normalised

        # Build merged event stream sorted by ts_ns. Each entry carries
        # symbol_id so step() knows which tape produced it.
        merged: List[Tuple[int, int, float, float, int]] = []
        for sym, tape in normalised.items():
            for ts, p, q, s in tape:
                merged.append((int(ts), sym, float(p), float(q), int(s)))
        merged.sort(key=lambda r: r[0])
        self._merged_events = merged
        # trades remains valid for length checks elsewhere
        self.trades = [
            (ts, p, q, s) for ts, _sym, p, q, s in merged
        ]

        # Per-symbol observation builders.
        self._multi_builders: Dict[int, ObservationBuilder] = {}
        for sym in normalised:
            self._multi_builders[sym] = ObservationBuilder(
                window_size=self.window_size,
                n_open_slots=int(self.n_open_slots),
                tick_size=self.tick_size,
                max_price_offset_ticks=self.max_price_offset_ticks,
                max_position=self.max_position
                if self.max_position
                else self.qty,
            )

        # Per-symbol Box action — same shape as single-symbol.
        offset = float(self.max_price_offset_ticks)
        per_symbol_action = _BoxSpace(
            low=[-1.0, -offset, 0.0],
            high=[1.0, offset, 2.0],
            shape=(3,),
            dtype="float32",
        )
        self.action_space = _DictSpace(
            spaces={str(sym): per_symbol_action for sym in normalised}
        )

        # Per-symbol Box observation — window + 2 + 4*n_open_slots.
        slot_count = int(self.n_open_slots)
        obs_dim = self.window_size + 2 + 4 * slot_count
        pos_bound = max(float(self.qty), float(self.max_position or self.qty))
        slot_low = [-1.0, 0.0, -1.0, 0.0] * slot_count
        slot_high = [1.0, 10.0, 1.0, 1.0] * slot_count
        per_symbol_obs = _BoxSpace(
            low=[0.0] * self.window_size + [-pos_bound, -1e9] + slot_low,
            high=[1e9] * self.window_size + [pos_bound, 1e9] + slot_high,
            shape=(obs_dim,),
            dtype="float32",
        )
        # Account-level slot: [equity, total_notional, total_unrealized_pnl].
        account_obs = _BoxSpace(
            low=[-1e12, 0.0, -1e12],
            high=[1e12, 1e12, 1e12],
            shape=(3,),
            dtype="float32",
        )
        spaces = {str(sym): per_symbol_obs for sym in normalised}
        spaces["account"] = account_obs
        self.observation_space = _DictSpace(spaces=spaces)

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
        tape: Optional[
            str | Path | Sequence[Tuple[int, float, float, int]]
        ] = None,
        tapes: Optional[
            Dict[Any, str | Path | Sequence[Tuple[int, float, float, int]]]
        ] = None,
        qty: float = 1.0,
        window_size: int = 8,
        symbol_id: int = 1,
        is_maker: bool = False,
        action_mode: str = "continuous",
        max_position: Optional[float] = None,
        tick_size: float = 0.01,
        max_price_offset_ticks: int = 50,
        n_open_slots: Optional[int] = None,
        reject_penalty: float = 0.0,
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
        if tapes is not None and tape is not None:
            raise ValueError("pass either `tape` (single-symbol) or `tapes` (multi-symbol), not both")
        if tapes is not None:
            resolved: Dict[int, List[Tuple[int, float, float, int]]] = {}
            for k, v in tapes.items():
                resolved[int(k)] = (
                    _load_tape_trades(v)
                    if isinstance(v, (str, Path))
                    else list(v)
                )
            return cls(
                trades=[],  # placeholder; multi mode rebuilds from merged
                multi_tapes=resolved,
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
                n_open_slots=n_open_slots,
                reject_penalty=reject_penalty,
            )

        if tape is None:
            raise ValueError("from_venue_stack needs either `tape` or `tapes`")
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
            n_open_slots=n_open_slots,
            reject_penalty=reject_penalty,
        )

    # ── Gymnasium API ────────────────────────────────────────────

    def reset(
        self, *, seed: Optional[int] = None, options: Optional[dict] = None
    ) -> Tuple[Any, dict]:
        if seed is not None:
            self.seed = seed
        self._idx = 0
        self._next_order_id = 0
        self._last_fill_idx = 0
        self._multi_last_fill_idx = 0
        self._last_outcome = {}

        if self.multi_tapes is not None:
            return self._reset_multi()

        self._position = 0.0
        self._entry_price = 0.0
        self._realized_pnl = 0.0
        self._last_total_pnl = 0.0
        self._venue_entry_price = 0.0
        self._open_orders = {}
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

    def step(self, action: Any) -> Tuple[Any, float, bool, bool, dict]:
        if self.multi_tapes is not None:
            return self._step_multi(action)
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

    def _reset_multi(self) -> Tuple[Any, dict]:
        self._multi_positions = {sym: 0.0 for sym in self.multi_tapes}
        self._multi_entry_prices = {sym: 0.0 for sym in self.multi_tapes}
        self._multi_open_orders = {sym: {} for sym in self.multi_tapes}
        for sym, builder in self._multi_builders.items():
            tape = self.multi_tapes[sym]
            first_price = float(tape[0][1]) if tape else 0.0
            builder.reset(first_price=first_price)
        if self.venue_stack is not None:
            acct = self.venue_stack.account()
            self._last_equity_at_mark = float(
                acct.equity() + acct.total_unrealised_pnl()
            )
        obs = self._observation_multi(
            event_price=float(self._merged_events[0][2])
            if self._merged_events
            else 0.0
        )
        info = {"step": 0, "positions": dict(self._multi_positions)}
        return obs, info

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

    def _step_multi(
        self, action: Any
    ) -> Tuple[Any, float, bool, bool, dict]:
        if not isinstance(action, dict):
            raise ValueError(
                f"multi-symbol action must be a dict; got {type(action).__name__}"
            )
        if self._idx >= len(self._merged_events):
            obs = self._observation_multi(event_price=0.0)
            return obs, 0.0, False, True, {"step": self._idx}

        ts_ns, sym_event, price_event, qty_event, side_event = (
            self._merged_events[self._idx]
        )

        stack = self.venue_stack
        exec_ = stack.executor()
        acct = stack.account()
        liq = stack.liquidation()
        fees = stack.fees()

        # 1. Decode actions per symbol; submit orders for non-zero
        # deltas. Symbols not present in the action dict default to a
        # hold.
        for sym in self.multi_tapes:
            sym_action = action.get(str(sym))
            if sym_action is None:
                continue
            try:
                signed_frac = float(sym_action[0])
                offset_ticks = float(sym_action[1])
                tif_axis = float(sym_action[2])
            except (TypeError, IndexError) as exc:
                raise ValueError(
                    f"action for symbol {sym} must have length 3"
                ) from exc

            signed_frac = max(-1.0, min(1.0, signed_frac))
            offset_int = int(round(offset_ticks))
            tif_int = int(round(max(0.0, min(2.0, tif_axis))))
            tif_name = {0: "gtc", 1: "ioc", 2: "post_only"}.get(tif_int, "gtc")

            target_pos = signed_frac * float(self.max_position)
            delta = target_pos - self._multi_positions[sym]
            if abs(delta) <= 1e-12:
                continue
            side = "buy" if delta > 0 else "sell"

            self._next_order_id += 1
            oid = self._next_order_id
            if offset_int == 0:
                # market — submit at current event price for that symbol
                latest_p = self._multi_builders[sym]._latest_price or price_event
                exec_.submit_order(
                    id=oid,
                    side=side,
                    price=float(latest_p),
                    quantity=abs(delta),
                    type="market",
                    symbol=sym,
                    tif="gtc",
                    account_id=acct.account_id(),
                )
            else:
                mid = self._multi_builders[sym]._latest_price or price_event
                side_sign = 1.0 if side == "buy" else -1.0
                limit_price = (
                    float(mid) + offset_int * float(self.tick_size) * side_sign
                )
                exec_.submit_order(
                    id=oid,
                    side=side,
                    price=float(limit_price),
                    quantity=abs(delta),
                    type="limit",
                    symbol=sym,
                    tif=tif_name,
                    account_id=acct.account_id(),
                )
                self._multi_open_orders[sym][oid] = {
                    "side": side, "type": "limit", "price": limit_price,
                    "qty_remaining": abs(delta), "submit_step": self._idx,
                    "tif": tif_name,
                }
                self._multi_builders[sym].add_open_order(
                    oid, side=side, order_type="limit",
                    price=limit_price, quantity=abs(delta), tif=tif_name,
                )

        # 2. Feed the current event's tick to the matching engine for
        # the symbol whose event this is.
        exec_.on_trade_qty(
            sym_event, float(price_event), float(qty_event), side_event == 0
        )

        # 3. Drain fills produced this step. Each fill carries
        # symbol; route bookkeeping to the right per-symbol state.
        all_fills = exec_.fills_list()
        new_fills = all_fills[self._multi_last_fill_idx :]
        self._multi_last_fill_idx = len(all_fills)
        for f in new_fills:
            fsym = int(f["symbol"])
            fill_qty = float(f["quantity"])
            fill_price = float(f["price"])
            signed = fill_qty if f["side"] == "buy" else -fill_qty
            self._apply_fill_multi(acct, fsym, signed, fill_price)
            notional = fill_price * fill_qty
            fees.record_fill(int(ts_ns), notional)
            fee = float(fees.fee_for(int(ts_ns), notional, self.is_maker))
            acct.add_equity(-fee)
            oid = int(f["order_id"])
            if oid in self._multi_open_orders.get(fsym, {}):
                self._multi_open_orders[fsym][oid]["qty_remaining"] -= fill_qty
                if (
                    self._multi_open_orders[fsym][oid]["qty_remaining"]
                    <= 1e-12
                ):
                    del self._multi_open_orders[fsym][oid]
                    self._multi_builders[fsym].remove_open_order(oid)
                else:
                    self._multi_builders[fsym].apply_fill(oid, fill_qty)

        # 4. Update mark for the event's symbol and run liquidation
        # walk on the cross-margin Account.
        acct.set_mark(sym_event, float(price_event), int(ts_ns))
        outcome = liq.on_mark(sym_event, float(price_event))
        self._last_outcome = outcome
        terminated = bool(outcome.get("liquidations_count", 0))

        # 5. Update per-symbol builder with the new price tick (only
        # the symbol whose event arrived; others keep their last
        # known price in the window).
        self._multi_builders[sym_event].on_trade(float(price_event))
        for sym in self.multi_tapes:
            self._multi_builders[sym].set_position(
                self._multi_positions[sym], self._multi_entry_prices[sym]
            )

        equity_now = float(acct.equity() + acct.total_unrealised_pnl())
        reward = equity_now - self._last_equity_at_mark
        self._last_equity_at_mark = equity_now
        if self.reward_fn is not None:
            ctx = {
                "ts_ns": ts_ns,
                "event_symbol": sym_event,
                "event_price": price_event,
                "positions": dict(self._multi_positions),
                "equity": acct.equity(),
                "unrealized_pnl": acct.total_unrealised_pnl(),
                "equity_at_mark": equity_now,
                "liquidation_outcome": outcome,
                "step": self._idx,
            }
            reward = float(self.reward_fn(self, ctx))

        self._idx += 1
        truncated = self._idx >= len(self._merged_events)
        obs = self._observation_multi(event_price=float(price_event))
        info = {
            "ts_ns": ts_ns,
            "event_symbol": sym_event,
            "event_price": price_event,
            "positions": dict(self._multi_positions),
            "equity": acct.equity(),
            "unrealized_pnl": acct.total_unrealised_pnl(),
            "equity_at_mark": equity_now,
            "fee_tier": fees.current_tier_index(),
            "liquidation_outcome": outcome,
            "step": self._idx,
        }
        return obs, reward, terminated, truncated, info

    def _apply_fill_multi(
        self, acct: Any, sym: int, signed: float, fill_price: float
    ) -> None:
        old_pos = self._multi_positions[sym]
        new_pos = old_pos + signed
        entry = self._multi_entry_prices[sym]

        if abs(old_pos) < 1e-12:
            if abs(new_pos) > 1e-12:
                acct.open_position(sym, new_pos, fill_price)
                self._multi_entry_prices[sym] = fill_price
            else:
                self._multi_entry_prices[sym] = 0.0
        elif old_pos * signed > 0:
            old_notional = entry * old_pos
            new_notional = fill_price * signed
            avg_entry = (old_notional + new_notional) / new_pos
            self._multi_entry_prices[sym] = avg_entry
            acct.close_position(sym)
            acct.open_position(sym, new_pos, avg_entry)
        elif old_pos * new_pos >= 0:
            direction = 1.0 if old_pos > 0 else -1.0
            realized = (fill_price - entry) * direction * abs(signed)
            acct.add_equity(realized)
            acct.close_position(sym)
            if abs(new_pos) > 1e-12:
                acct.open_position(sym, new_pos, entry)
            else:
                self._multi_entry_prices[sym] = 0.0
        else:
            direction = 1.0 if old_pos > 0 else -1.0
            realized = (fill_price - entry) * direction * abs(old_pos)
            acct.add_equity(realized)
            acct.close_position(sym)
            acct.open_position(sym, new_pos, fill_price)
            self._multi_entry_prices[sym] = fill_price
        self._multi_positions[sym] = new_pos

    def _observation_multi(self, *, event_price: float) -> Dict[str, Any]:
        out: Dict[str, Any] = {}
        for sym, builder in self._multi_builders.items():
            out[str(sym)] = builder.build()
        if self.venue_stack is not None:
            acct = self.venue_stack.account()
            out["account"] = [
                float(acct.equity()),
                float(acct.total_notional()),
                float(acct.total_unrealised_pnl()),
            ]
        else:
            out["account"] = [0.0, 0.0, 0.0]
        return out

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

        submitted_id: Optional[int] = None
        if abs(delta_qty) > 1e-12:
            side = "buy" if delta_qty > 0 else "sell"
            self._next_order_id += 1
            submitted_id = self._next_order_id
            submit_price = (
                float(limit_price) if limit_price is not None else float(price)
            )
            exec_.submit_order(
                id=submitted_id,
                side=side,
                price=submit_price,
                quantity=abs(delta_qty),
                type=order_type,
                symbol=self.symbol_id,
                tif=tif,
                account_id=acct.account_id(),
            )
            self._open_orders[submitted_id] = {
                "side": side,
                "type": order_type,
                "price": submit_price,
                "qty_remaining": abs(delta_qty),
                "initial_qty": abs(delta_qty),
                "submit_step": self._idx,
                "tif": tif,
            }

        exec_.on_trade_qty(
            self.symbol_id, float(price), float(trade_qty), trade_side == 0
        )

        # Drain fills produced during this step.
        all_fills = exec_.fills_list()
        new_fills = all_fills[self._last_fill_idx :]
        self._last_fill_idx = len(all_fills)
        filled_this_step: Dict[int, float] = {}
        for f in new_fills:
            fill_qty = float(f["quantity"])
            fill_price = float(f["price"])
            signed = fill_qty if f["side"] == "buy" else -fill_qty
            self._apply_fill(acct, signed, fill_price)

            # Record notional for tier tracking and deduct the fee from
            # account equity.
            notional = fill_price * fill_qty
            fees.record_fill(int(ts_ns), notional)
            fee = float(fees.fee_for(int(ts_ns), notional, self.is_maker))
            acct.add_equity(-fee)

            oid = int(f["order_id"])
            filled_this_step[oid] = filled_this_step.get(oid, 0.0) + fill_qty
            if oid in self._open_orders:
                self._open_orders[oid]["qty_remaining"] -= fill_qty
                if self._open_orders[oid]["qty_remaining"] <= 1e-12:
                    del self._open_orders[oid]

        # Reject heuristic: we submitted an order this step but it
        # neither filled nor sits in _open_orders any more. That happens
        # when the executor rejected it pre-book (rate limit, venue
        # outage, post-only cross). Penalise it once.
        rejected_this_step = False
        if (
            submitted_id is not None
            and submitted_id not in self._open_orders
            and submitted_id not in filled_this_step
        ):
            rejected_this_step = True

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
        if rejected_this_step and self.reject_penalty:
            reward_default -= float(self.reject_penalty)
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
            "open_orders": len(self._open_orders),
            "rejected": rejected_this_step,
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
        return list(normalized) + [self._position, unreal] + self._open_order_slots()

    def _open_order_slots(self) -> List[float]:
        n = int(self.n_open_slots)
        if n <= 0:
            return []

        cur_price = (
            float(self.trades[self._idx][1])
            if self._idx < len(self.trades)
            else self._first_price
        )
        max_offset_ticks = max(int(self.max_price_offset_ticks), 1)
        tick = float(self.tick_size) if self.tick_size > 0 else 1.0
        norm_qty = float(self.max_position) if self.max_position > 0 else 1.0
        window = max(int(self.window_size), 1)

        items = sorted(
            self._open_orders.items(), key=lambda kv: kv[1]["submit_step"]
        )[:n]

        slots: List[float] = []
        for _, od in items:
            qty_signed = od["qty_remaining"] * (1.0 if od["side"] == "buy" else -1.0)
            age = max(0, self._idx - od["submit_step"])
            distance_ticks = (od["price"] - cur_price) / tick
            queue_pos = 0.0 if od["type"] == "market" else 0.5
            slots.extend([
                qty_signed / norm_qty,
                age / window,
                max(-1.0, min(1.0, distance_ticks / max_offset_ticks)),
                queue_pos,
            ])

        # Pad unused slots with zeros so the obs shape is constant.
        slots.extend([0.0] * (4 * (n - len(items))))
        return slots


# ── Reusable observation builder and action decoder ──────────────


class ObservationBuilder:
    """Stateful builder that turns a stream of trades + a current
    position into the same observation vector ``FloxTradingEnv``
    serves. Used by ``RLPolicy`` so a trained model sees the same
    obs shape and semantics in env, paper, and live.

    The price window is filled lazily — the first observed price
    anchors normalisation, matching the env's ``reset`` behaviour.
    Open orders are tracked by ID via ``add_open_order`` /
    ``remove_open_order`` / ``apply_fill`` so the policy keeps the
    same slot ordering the env used during training.
    """

    def __init__(
        self,
        *,
        window_size: int = 8,
        n_open_slots: int = 0,
        tick_size: float = 0.01,
        max_price_offset_ticks: int = 50,
        max_position: float = 1.0,
    ) -> None:
        self.window_size = max(1, int(window_size))
        self.n_open_slots = max(0, int(n_open_slots))
        self.tick_size = float(tick_size) if tick_size > 0 else 1.0
        self.max_price_offset_ticks = max(1, int(max_price_offset_ticks))
        self.max_position = float(max_position) if max_position > 0 else 1.0

        self._price_window: Deque[float] = deque()
        self._first_price: float = 0.0
        self._latest_price: float = 0.0
        self._position: float = 0.0
        self._entry_price: float = 0.0
        self._open_orders: Dict[int, dict] = {}
        self._step: int = 0

    def reset(self, first_price: Optional[float] = None) -> None:
        self._price_window.clear()
        if first_price is not None:
            self._first_price = float(first_price)
            self._latest_price = float(first_price)
            for _ in range(self.window_size):
                self._price_window.append(float(first_price))
        else:
            self._first_price = 0.0
            self._latest_price = 0.0
        self._position = 0.0
        self._entry_price = 0.0
        self._open_orders.clear()
        self._step = 0

    def on_trade(self, price: float) -> None:
        if self._first_price == 0.0:
            self._first_price = float(price)
            for _ in range(self.window_size):
                self._price_window.append(float(price))
        self._latest_price = float(price)
        self._price_window.append(float(price))
        if len(self._price_window) > self.window_size:
            self._price_window.popleft()
        self._step += 1

    def set_position(self, position: float, entry_price: float = 0.0) -> None:
        self._position = float(position)
        self._entry_price = float(entry_price)

    def add_open_order(
        self,
        order_id: int,
        *,
        side: str,
        order_type: str,
        price: float,
        quantity: float,
        tif: str = "gtc",
    ) -> None:
        self._open_orders[int(order_id)] = {
            "side": side,
            "type": order_type,
            "price": float(price),
            "qty_remaining": float(quantity),
            "initial_qty": float(quantity),
            "submit_step": self._step,
            "tif": tif,
        }

    def apply_fill(self, order_id: int, fill_qty: float) -> None:
        oid = int(order_id)
        if oid not in self._open_orders:
            return
        self._open_orders[oid]["qty_remaining"] -= float(fill_qty)
        if self._open_orders[oid]["qty_remaining"] <= 1e-12:
            del self._open_orders[oid]

    def remove_open_order(self, order_id: int) -> None:
        self._open_orders.pop(int(order_id), None)

    def build(self) -> List[float]:
        denom = self._first_price if self._first_price > 0 else 1.0
        normalized = [p / denom for p in self._price_window]
        unreal = (
            (self._latest_price - self._entry_price) * self._position
            if self._position != 0.0
            else 0.0
        )
        slots: List[float] = []
        if self.n_open_slots > 0:
            items = sorted(
                self._open_orders.items(),
                key=lambda kv: kv[1]["submit_step"],
            )[: self.n_open_slots]
            for _, od in items:
                qty_signed = od["qty_remaining"] * (
                    1.0 if od["side"] == "buy" else -1.0
                )
                age = max(0, self._step - od["submit_step"])
                distance_ticks = (od["price"] - self._latest_price) / self.tick_size
                queue_pos = 0.0 if od["type"] == "market" else 0.5
                slots.extend([
                    qty_signed / self.max_position,
                    age / self.window_size,
                    max(-1.0, min(1.0, distance_ticks / self.max_price_offset_ticks)),
                    queue_pos,
                ])
            slots.extend([0.0] * (4 * (self.n_open_slots - len(items))))
        return list(normalized) + [self._position, unreal] + slots


class ActionDecoder:
    """Decode a continuous Box((3,)) action into a structured order
    intent the policy can emit through a broker. The decoded fields
    mirror the env's interpretation: signed_qty is target position
    expressed as a fraction of ``max_position``; price offset of 0
    means market; TIF rounds to {GTC, IOC, Post-only}.
    """

    _TIF_BY_INDEX = {0: "gtc", 1: "ioc", 2: "post_only"}

    def __init__(
        self,
        *,
        max_position: float = 1.0,
        tick_size: float = 0.01,
        max_price_offset_ticks: int = 50,
    ) -> None:
        self.max_position = float(max_position)
        self.tick_size = float(tick_size)
        self.max_price_offset_ticks = max(1, int(max_price_offset_ticks))

    def decode(
        self, action: Sequence[float], mid_price: float, current_position: float
    ) -> dict:
        signed_frac = max(-1.0, min(1.0, float(action[0])))
        offset_ticks = max(
            -float(self.max_price_offset_ticks),
            min(float(self.max_price_offset_ticks), float(action[1])),
        )
        tif_axis = max(0.0, min(2.0, float(action[2])))
        target_position = signed_frac * self.max_position
        delta = target_position - float(current_position)
        offset_int = int(round(offset_ticks))
        tif_int = int(round(tif_axis))
        if abs(delta) < 1e-12:
            return {
                "order_type": "hold",
                "side": None,
                "quantity": 0.0,
                "price": None,
                "tif": "gtc",
                "target_position": target_position,
                "delta": 0.0,
            }
        side = "buy" if delta > 0 else "sell"
        if offset_int == 0:
            return {
                "order_type": "market",
                "side": side,
                "quantity": abs(delta),
                "price": None,
                "tif": "gtc",
                "target_position": target_position,
                "delta": delta,
            }
        side_sign = 1.0 if side == "buy" else -1.0
        limit_price = float(mid_price) + offset_int * self.tick_size * side_sign
        return {
            "order_type": "limit",
            "side": side,
            "quantity": abs(delta),
            "price": limit_price,
            "tif": self._TIF_BY_INDEX.get(tif_int, "gtc"),
            "target_position": target_position,
            "delta": delta,
        }


# ── RLPolicy adapter ──────────────────────────────────────────────


def _import_strategy_base() -> Any:
    """flox_py.Strategy is a C++-bound class. Importing it at module
    load time would force every user of rl_env to pay for the engine
    bindings even when they only use FloxTradingEnv. We resolve it
    lazily so RLPolicy stays optional."""
    import flox_py
    return flox_py.Strategy


def make_rl_policy(
    model: Any,
    symbol_id: int,
    *,
    observation_builder: ObservationBuilder,
    action_decoder: ActionDecoder,
    deterministic: bool = True,
) -> Any:
    """Build a ``flox.Strategy`` subclass instance that runs a trained
    RL policy live. The same instance plugs into ``PaperBroker``
    (live feed → simulated fills) and ``CcxtBroker`` (real exchange)
    via the standard ``Runner.add_strategy`` flow — the strategy code
    is the model + the builder + the decoder, nothing else changes
    between modes.

    ``model`` must expose ``.predict(observation, deterministic=...) ->
    (action, state)``. Stable-Baselines3, RLlib, and CleanRL all
    satisfy that contract.
    """
    Strategy = _import_strategy_base()

    class _RLPolicy(Strategy):
        def __init__(self) -> None:
            super().__init__([int(symbol_id)])
            self.model = model
            self.observation_builder = observation_builder
            self.action_decoder = action_decoder
            self.deterministic = bool(deterministic)
            self.symbol_id = int(symbol_id)
            self._initialised = False

        def on_start(self) -> None:
            if not self._initialised:
                self.observation_builder.reset()
                self._initialised = True

        def on_stop(self) -> None:
            pass

        def on_trade(self, ctx, trade) -> None:
            price = float(trade.price)
            self.observation_builder.on_trade(price)
            self.observation_builder.set_position(
                float(getattr(ctx, "position", 0.0)),
                float(getattr(ctx, "avg_entry_price", 0.0)),
            )
            obs = self.observation_builder.build()
            action, _state = self.model.predict(
                obs, deterministic=self.deterministic
            )
            decoded = self.action_decoder.decode(
                action, price, float(getattr(ctx, "position", 0.0))
            )
            self._emit(decoded)

        def _emit(self, decoded: dict) -> None:
            qty = float(decoded["quantity"])
            if qty <= 0.0 or decoded["order_type"] == "hold":
                return
            side = decoded["side"]
            otype = decoded["order_type"]
            tif = decoded["tif"]
            if otype == "market":
                if side == "buy":
                    self.emit_market_buy(self.symbol_id, qty)
                else:
                    self.emit_market_sell(self.symbol_id, qty)
                return
            # limit
            price = float(decoded["price"])
            if side == "buy":
                self.emit_limit_buy_tif(self.symbol_id, price, qty, tif=tif)
            else:
                self.emit_limit_sell_tif(self.symbol_id, price, qty, tif=tif)

    return _RLPolicy()


# ── Walk-forward harness ──────────────────────────────────────────


_NS_PER_DAY = 86_400 * 1_000_000_000


@dataclass
class _Fold:
    index: int
    train_range: Tuple[int, int]
    test_range: Tuple[int, int]


class WalkForwardRL:
    """Walk-forward training and evaluation harness for
    ``FloxTradingEnv``. Splits a tape into ``n_folds`` train / test
    windows (anchored — expanding train, fixed-size test — or
    sliding — both windows slide). A fresh ``VenueStack`` is built
    per fold via the user-supplied factory so no fee / funding /
    notional / liquidation state leaks across folds.

    Output schema mirrors the supervised ``WalkForwardRunner`` so RL
    and non-RL sweeps land in the same comparison tables.

    ```python
    wf = WalkForwardRL(
        venue_stack_factory=lambda: flox.VenueStack.binance_um_futures(42, 10_000.0),
        tape=trades,                    # path or list of (ts_ns, price, qty, side)
        train_window_days=14,
        test_window_days=3,
        n_folds=6,
        mode="anchored",
        env_kwargs={"qty": 0.01, "max_position": 0.05, "window_size": 32},
    )

    for train_env, test_env in wf:
        model = PPO("MlpPolicy", train_env).learn(100_000)
        metrics = wf.evaluate(model, test_env)

    aggregate = wf.aggregate()
    print(aggregate["mean_return_pct"], aggregate["sign_match"])
    ```
    """

    def __init__(
        self,
        *,
        venue_stack_factory: Callable[[], Any],
        tape: str | Path | Sequence[Tuple[int, float, float, int]],
        train_window_days: float,
        test_window_days: float,
        n_folds: int,
        mode: str = "anchored",
        env_kwargs: Optional[dict] = None,
        symbol_id: int = 1,
    ) -> None:
        if mode not in ("anchored", "sliding"):
            raise ValueError(
                f"mode must be 'anchored' or 'sliding'; got {mode!r}"
            )
        if n_folds <= 0:
            raise ValueError(f"n_folds must be positive; got {n_folds}")
        if train_window_days <= 0 or test_window_days <= 0:
            raise ValueError("window days must be positive")

        self.venue_stack_factory = venue_stack_factory
        self.train_window_days = float(train_window_days)
        self.test_window_days = float(test_window_days)
        self.n_folds = int(n_folds)
        self.mode = mode
        self.env_kwargs = dict(env_kwargs or {})
        self.symbol_id = int(symbol_id)

        self._trades = (
            _load_tape_trades(tape)
            if isinstance(tape, (str, Path))
            else list(tape)
        )
        if not self._trades:
            raise ValueError("WalkForwardRL needs a non-empty tape")

        self._folds = self._build_folds()
        self._metrics: List[dict] = []

    def _build_folds(self) -> List[_Fold]:
        train_ns = int(self.train_window_days * _NS_PER_DAY)
        test_ns = int(self.test_window_days * _NS_PER_DAY)
        first_ts = int(self._trades[0][0])
        last_ts = int(self._trades[-1][0])
        total_ns = last_ts - first_ts
        if total_ns < train_ns + test_ns:
            raise ValueError(
                f"tape spans {total_ns / _NS_PER_DAY:.2f} days; need at "
                f"least train+test = {(train_ns + test_ns) / _NS_PER_DAY:.2f}"
            )

        # Each fold advances by test_ns. Anchored mode pins train start
        # at first_ts; sliding mode advances train start by test_ns too.
        folds: List[_Fold] = []
        for k in range(self.n_folds):
            test_start = first_ts + train_ns + k * test_ns
            test_end = test_start + test_ns
            if test_end > last_ts:
                break
            if self.mode == "anchored":
                train_start = first_ts
            else:
                train_start = first_ts + k * test_ns
            train_end = test_start
            folds.append(
                _Fold(
                    index=k,
                    train_range=(train_start, train_end),
                    test_range=(test_start, test_end),
                )
            )
        if not folds:
            raise ValueError(
                "tape too short to produce a single fold at the configured "
                "window sizes"
            )
        return folds

    def _slice_trades(
        self, lo_ts: int, hi_ts: int
    ) -> List[Tuple[int, float, float, int]]:
        return [t for t in self._trades if lo_ts <= int(t[0]) < hi_ts]

    def _make_env(
        self, sub_trades: Sequence[Tuple[int, float, float, int]]
    ) -> "FloxTradingEnv":
        stack = self.venue_stack_factory()
        return FloxTradingEnv.from_venue_stack(
            stack,
            tape=sub_trades,
            symbol_id=self.symbol_id,
            **self.env_kwargs,
        )

    def __iter__(self) -> Any:
        for fold in self._folds:
            train_slice = self._slice_trades(*fold.train_range)
            test_slice = self._slice_trades(*fold.test_range)
            if not train_slice or not test_slice:
                continue
            train_env = self._make_env(train_slice)
            test_env = self._make_env(test_slice)
            yield train_env, test_env

    def evaluate(self, model: Any, test_env: "FloxTradingEnv") -> dict:
        """Run the model deterministically through one test env and
        return a per-fold metrics dict. The dict shape mirrors the
        supervised walk-forward output so cross-mode aggregations are
        possible."""
        obs, _ = test_env.reset(seed=0)
        equities: List[float] = []
        total_reward = 0.0
        steps = 0
        terminated = False
        truncated = False
        while not (terminated or truncated):
            action, _ = model.predict(obs, deterministic=True)
            obs, reward, terminated, truncated, info = test_env.step(action)
            total_reward += float(reward)
            steps += 1
            equity_at_mark = info.get("equity_at_mark", info.get("equity"))
            if equity_at_mark is not None:
                equities.append(float(equity_at_mark))

        if equities:
            start_eq = equities[0]
            end_eq = equities[-1]
            return_pct = (
                (end_eq - start_eq) / start_eq * 100.0 if start_eq != 0 else 0.0
            )
            peak = start_eq
            max_dd = 0.0
            for e in equities:
                peak = max(peak, e)
                dd = (peak - e) / peak if peak > 0 else 0.0
                max_dd = max(max_dd, dd)
            mean_reward = total_reward / max(steps, 1)
            variance = (
                sum((r - mean_reward) ** 2 for r in equities) / max(steps, 1)
            )
            std = variance ** 0.5
            sharpe = (
                (mean_reward / std) if std > 0 else 0.0
            )
        else:
            return_pct = 0.0
            max_dd = 0.0
            sharpe = 0.0

        m = {
            "fold": len(self._metrics),
            "return_pct": return_pct,
            "max_drawdown_pct": max_dd * 100.0,
            "sharpe": sharpe,
            "total_reward": total_reward,
            "steps": steps,
        }
        self._metrics.append(m)
        return m

    def aggregate(self) -> dict:
        if not self._metrics:
            return {
                "n_folds": 0,
                "mean_return_pct": 0.0,
                "std_return_pct": 0.0,
                "sign_match": 0.0,
                "worst_return_pct": 0.0,
                "mean_sharpe": 0.0,
                "mean_max_drawdown_pct": 0.0,
            }
        returns = [m["return_pct"] for m in self._metrics]
        sharpes = [m["sharpe"] for m in self._metrics]
        dds = [m["max_drawdown_pct"] for m in self._metrics]
        n = len(returns)
        mean_r = sum(returns) / n
        var_r = sum((r - mean_r) ** 2 for r in returns) / max(n - 1, 1)
        std_r = var_r ** 0.5
        sign_match = sum(1 for r in returns if r > 0) / n
        worst = min(returns)
        mean_sharpe = sum(sharpes) / n
        mean_dd = sum(dds) / n
        return {
            "n_folds": n,
            "mean_return_pct": mean_r,
            "std_return_pct": std_r,
            "sign_match": sign_match,
            "worst_return_pct": worst,
            "mean_sharpe": mean_sharpe,
            "mean_max_drawdown_pct": mean_dd,
        }


__all__ = [
    "FloxTradingEnv",
    "ObservationBuilder",
    "ActionDecoder",
    "make_rl_policy",
    "WalkForwardRL",
]
