"""Composable execution algorithms.

Strategies emit a target intent ("buy 1.0 BTC over the next hour"
or "sell into 5 percent of market volume"). The execution algo
turns that intent into a stream of child orders submitted through
a regular ``Executor``. Each algo is a small stateful object the
user app drives from a tick or a timer; nothing in flox engine
internals changes.

Four built-in algos in this module:

* ``TWAPExecutor`` — equal-time slicing across a fixed duration.
* ``VWAPExecutor`` — slice sizes follow a supplied volume curve.
* ``IcebergExecutor`` — show only ``visible_qty`` at a time;
  resubmit the next slice when the previous one fills.
* ``POVExecutor`` — chase a fixed share of observed market volume.

Each algo speaks the same minimal executor contract:

* ``submit_order(id, side, price, qty, type, symbol)`` — same
  signature as ``flox_py.SimulatedExecutor`` and the live
  ``IOrderExecutor`` C++ surface.

That keeps backtest and live use the same: pass the simulator in
backtest, pass your live broker executor in production. Any object
that implements ``submit_order`` works.

These are Python-side primitives. The C++ engine has the same
``ExecutionListener`` hook surface; cross-binding parity for the
algo classes themselves is a Phase 2 follow-up.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any, Callable, List, Optional, Sequence


_VALID_SIDES = ("buy", "sell")
_VALID_TYPES = ("market", "limit")


def _validate_side(side: str) -> str:
    s = (side or "").lower()
    if s not in _VALID_SIDES:
        raise ValueError(f"side must be 'buy' or 'sell'; got {side!r}")
    return s


def _validate_type(t: str) -> str:
    s = (t or "").lower()
    if s not in _VALID_TYPES:
        raise ValueError(f"order type must be 'market' or 'limit'; got {t!r}")
    return s


def _validate_positive(name: str, value: float) -> None:
    if not (value > 0.0):
        raise ValueError(f"{name} must be positive; got {value}")


@dataclass
class ChildOrder:
    """Record of one slice the algo submitted."""

    order_id: int
    timestamp_ns: int
    qty: float
    price: float
    type: str


@dataclass
class _BaseExecutor:
    """Internal base. Provides shared state plus child-order tracking."""

    target_qty: float
    side: str
    symbol: int
    type: str = "market"
    price: float = 0.0
    next_order_id: int = 1
    _filled_qty: float = field(default=0.0, init=False)
    _submitted_qty: float = field(default=0.0, init=False)
    _children: List[ChildOrder] = field(default_factory=list, init=False)

    def __post_init__(self) -> None:
        _validate_positive("target_qty", self.target_qty)
        self.side = _validate_side(self.side)
        self.type = _validate_type(self.type)
        if self.type == "limit":
            _validate_positive("price", self.price)

    @property
    def submitted_qty(self) -> float:
        return self._submitted_qty

    @property
    def filled_qty(self) -> float:
        return self._filled_qty

    @property
    def remaining_qty(self) -> float:
        return max(0.0, float(self.target_qty) - self._submitted_qty)

    @property
    def children(self) -> List[ChildOrder]:
        return list(self._children)

    def is_done(self) -> bool:
        # Floating-point-safe completion check.
        return self.remaining_qty <= 1e-9

    def report_fill(self, qty: float) -> None:
        """User app calls this when a child order fills (live or sim).
        Several algos use filled_qty to gate the next slice."""
        if qty < 0:
            raise ValueError(f"fill qty must be non-negative; got {qty}")
        self._filled_qty += float(qty)

    def _submit_through(
        self,
        executor: Any,
        *,
        qty: float,
        now_ns: int,
        order_type: Optional[str] = None,
        price: Optional[float] = None,
    ) -> ChildOrder:
        if qty <= 0:
            raise ValueError(f"slice qty must be positive; got {qty}")
        oid = int(self.next_order_id)
        self.next_order_id += 1
        ot = order_type or self.type
        pr = float(price if price is not None else self.price)
        executor.submit_order(
            oid, self.side, pr, float(qty),
            type=ot, symbol=int(self.symbol),
        )
        rec = ChildOrder(
            order_id=oid, timestamp_ns=int(now_ns),
            qty=float(qty), price=pr, type=ot,
        )
        self._children.append(rec)
        self._submitted_qty += float(qty)
        return rec


# ── TWAP ──────────────────────────────────────────────────────────


@dataclass
class TWAPExecutor(_BaseExecutor):
    """Equal-time slicing across a fixed duration. The algo emits
    one child of size ``target_qty / slice_count`` at each scheduled
    tick. ``step(now_ns, executor)`` is the user-facing entry point;
    call from a periodic timer aligned with ``slice_interval_ns``."""

    duration_ns: int = 0
    slice_count: int = 0
    start_time_ns: int = 0
    _next_slice_idx: int = field(default=0, init=False)

    def __post_init__(self) -> None:
        super().__post_init__()
        if self.slice_count <= 0:
            raise ValueError(
                f"slice_count must be > 0; got {self.slice_count}"
            )
        if self.duration_ns <= 0:
            raise ValueError(
                f"duration_ns must be > 0; got {self.duration_ns}"
            )

    @property
    def slice_interval_ns(self) -> int:
        return int(self.duration_ns // self.slice_count)

    @property
    def slice_size(self) -> float:
        return float(self.target_qty) / float(self.slice_count)

    def step(self, now_ns: int, executor: Any) -> List[ChildOrder]:
        """Emit any child orders whose scheduled time has passed.
        Returns the list of children submitted in this call."""
        out: List[ChildOrder] = []
        while (
            self._next_slice_idx < self.slice_count
            and now_ns >= self.start_time_ns + self._next_slice_idx * self.slice_interval_ns
        ):
            qty = min(self.slice_size, self.remaining_qty)
            if qty <= 0:
                break
            out.append(self._submit_through(executor, qty=qty, now_ns=now_ns))
            self._next_slice_idx += 1
        return out


# ── VWAP ──────────────────────────────────────────────────────────


@dataclass
class VWAPExecutor(_BaseExecutor):
    """Slice sizes follow a supplied volume curve. ``volume_curve``
    is a list of ``(bar_ts_ns, volume)`` rows ordered by time; each
    bar's slice gets a share of ``target_qty`` proportional to its
    volume relative to the total. The algo emits one slice per
    elapsed bar."""

    volume_curve: Sequence[tuple[int, float]] = field(default_factory=list)
    _bar_idx: int = field(default=0, init=False)
    _total_volume: float = field(default=0.0, init=False)

    def __post_init__(self) -> None:
        super().__post_init__()
        if not self.volume_curve:
            raise ValueError("VWAPExecutor needs a non-empty volume_curve")
        for ts, vol in self.volume_curve:
            if vol < 0:
                raise ValueError(
                    f"volume_curve volume must be non-negative; got {vol}"
                )
        self._total_volume = sum(float(v) for _, v in self.volume_curve)
        if self._total_volume <= 0:
            raise ValueError(
                "volume_curve total volume must be positive"
            )

    def step(self, now_ns: int, executor: Any) -> List[ChildOrder]:
        out: List[ChildOrder] = []
        while self._bar_idx < len(self.volume_curve):
            bar_ts, bar_vol = self.volume_curve[self._bar_idx]
            if now_ns < int(bar_ts):
                break
            self._bar_idx += 1
            if bar_vol <= 0:
                continue
            share = float(bar_vol) / self._total_volume
            qty = min(share * float(self.target_qty), self.remaining_qty)
            if qty <= 0:
                break
            out.append(self._submit_through(executor, qty=qty, now_ns=now_ns))
        return out


# ── Iceberg ────────────────────────────────────────────────────────


@dataclass
class IcebergExecutor(_BaseExecutor):
    """Display only ``visible_qty`` at a time. The algo submits one
    child of ``visible_qty`` (or whatever remains, if less); the
    user reports fills through ``report_fill``; once the visible
    slice is filled the next slice is submitted."""

    visible_qty: float = 0.0
    _last_submitted_id: int = field(default=0, init=False)

    def __post_init__(self) -> None:
        super().__post_init__()
        _validate_positive("visible_qty", self.visible_qty)
        if self.visible_qty > self.target_qty:
            raise ValueError(
                "visible_qty must not exceed target_qty"
            )

    def step(self, now_ns: int, executor: Any) -> List[ChildOrder]:
        # Only submit a new child when nothing is outstanding (i.e.
        # filled_qty has caught up to submitted_qty).
        if self.is_done():
            return []
        outstanding = self._submitted_qty - self._filled_qty
        if outstanding > 1e-9:
            return []
        slice_qty = min(float(self.visible_qty), self.remaining_qty)
        if slice_qty <= 0:
            return []
        return [self._submit_through(executor, qty=slice_qty, now_ns=now_ns)]


# ── POV ───────────────────────────────────────────────────────────


@dataclass
class POVExecutor(_BaseExecutor):
    """Percent-of-Volume. Tracks observed market volume and submits
    slices so cumulative submitted qty stays close to
    ``participation_rate * cumulative observed volume``. The user
    feeds market volume via ``observe_volume``."""

    participation_rate: float = 0.0
    min_slice_qty: float = 0.0
    _observed_volume: float = field(default=0.0, init=False)

    def __post_init__(self) -> None:
        super().__post_init__()
        if not (0.0 < self.participation_rate <= 1.0):
            raise ValueError(
                "participation_rate must be in (0, 1]; got "
                f"{self.participation_rate}"
            )
        if self.min_slice_qty < 0:
            raise ValueError(
                f"min_slice_qty must be non-negative; got {self.min_slice_qty}"
            )

    def observe_volume(self, qty: float) -> None:
        if qty < 0:
            raise ValueError(f"observed volume must be non-negative; got {qty}")
        self._observed_volume += float(qty)

    def step(self, now_ns: int, executor: Any) -> List[ChildOrder]:
        if self.is_done():
            return []
        target = self.participation_rate * self._observed_volume
        slice_qty = target - self._submitted_qty
        if slice_qty < self.min_slice_qty:
            return []
        slice_qty = min(slice_qty, self.remaining_qty)
        if slice_qty <= 0:
            return []
        return [self._submit_through(executor, qty=slice_qty, now_ns=now_ns)]


__all__ = [
    "ChildOrder",
    "TWAPExecutor",
    "VWAPExecutor",
    "IcebergExecutor",
    "POVExecutor",
]
