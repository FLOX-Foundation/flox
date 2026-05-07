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

Phase 1 deliberately stays narrow: trade-by-trade replay, scalar
qty, fixed flat-or-long-or-short discrete actions. Continuous
action spaces, per-symbol multi-instrument portfolios, and order
types beyond market are Phase 2 follow-ups.
"""
from __future__ import annotations

import math
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

    Reward is the PnL delta (realized + unrealized) since the
    previous step. Override ``reward_fn`` to customize.
    """

    trades: Sequence[Tuple[int, float, float, int]] = field(default_factory=list)
    qty: float = 1.0
    window_size: int = 8
    reward_fn: Optional[Callable[["FloxTradingEnv", Any], float]] = None
    seed: Optional[int] = None

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
        ``.floxlog`` tape directory."""
        return cls(
            trades=_load_tape_trades(path),
            qty=qty,
            window_size=window_size,
            reward_fn=reward_fn,
            seed=seed,
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
        self._price_window.clear()
        # Pre-fill the window with the first trade's price so the
        # observation has a defined shape on the first step.
        first_price = float(self.trades[0][1])
        self._first_price = first_price
        for _ in range(self.window_size):
            self._price_window.append(first_price)
        obs = self._observation()
        info = {"step": self._idx, "position": self._position}
        return obs, info

    def step(self, action: int) -> Tuple[List[float], float, bool, bool, dict]:
        if not self.action_space.contains(int(action)):
            raise ValueError(
                f"action {action!r} not in action_space (n=3)"
            )

        ts_ns, price, _, _ = self.trades[self._idx]
        self._price_window.append(float(price))
        if len(self._price_window) > self.window_size:
            self._price_window.popleft()

        # Apply action: switch position to {long, short, flat}.
        target_position = {0: self._position, 1: float(self.qty), 2: -float(self.qty)}[int(action)]
        if target_position != self._position:
            # Close existing, open new at current price.
            if self._position != 0.0:
                pnl_close = (price - self._entry_price) * self._position
                self._realized_pnl += pnl_close
            self._position = target_position
            self._entry_price = float(price) if target_position != 0.0 else 0.0

        unrealized = (price - self._entry_price) * self._position if self._position != 0.0 else 0.0
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

    def render(self) -> None:
        # Render mode is intentionally a no-op for Phase 1; the
        # tooling that visualizes trades belongs to the replay viewer.
        pass

    def close(self) -> None:
        # Nothing to close; the trade list is plain Python data.
        pass

    # ── Internal ─────────────────────────────────────────────────

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
