"""Pluggable execution algorithms (TWAP, VWAP, Iceberg, POV).

The C++ engine carries the state machine; this module preserves the
existing Python class shape (TWAPExecutor / VWAPExecutor /
IcebergExecutor / POVExecutor) so callers do not change. ``step``
calls into the engine, then dispatches the freshly emitted child
orders through the user-supplied ``executor.submit_order`` API.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any, List, Optional, Sequence


_VALID_SIDES = {"buy", "sell"}
_VALID_TYPES = {"market", "limit"}


def _validate_side(side: str) -> str:
    s = str(side).lower()
    if s not in _VALID_SIDES:
        raise ValueError(f"side must be one of {_VALID_SIDES}; got {side!r}")
    return s


def _validate_type(t: str) -> str:
    s = str(t).lower()
    if s not in _VALID_TYPES:
        raise ValueError(f"type must be one of {_VALID_TYPES}; got {t!r}")
    return s


def _validate_positive(name: str, value: float) -> None:
    if value <= 0:
        raise ValueError(f"{name} must be positive; got {value}")


@dataclass
class ChildOrder:
    """Record of one slice the algo submitted."""

    order_id: int
    timestamp_ns: int
    qty: float
    price: float
    type: str


def _side_enum(side: str) -> Any:
    from flox_py._flox_py import _ExecSide  # type: ignore[attr-defined]
    return _ExecSide.Buy if side == "buy" else _ExecSide.Sell


def _type_enum(t: str) -> Any:
    from flox_py._flox_py import _ExecOrderType  # type: ignore[attr-defined]
    return _ExecOrderType.Market if t == "market" else _ExecOrderType.Limit


def _drain_pending(native: Any, executor: Any, side: str,
                   symbol: int) -> List[ChildOrder]:
    """Pull child orders the engine emitted on the last `step`, ship
    them through the user's executor, then clear the engine buffer."""
    out: List[ChildOrder] = []
    for entry in native.pending():
        rec = ChildOrder(
            order_id=int(entry["order_id"]),
            timestamp_ns=int(entry["timestamp_ns"]),
            qty=float(entry["qty"]),
            price=float(entry["price"]),
            type=str(entry["type"]),
        )
        executor.submit_order(
            rec.order_id, side, rec.price, rec.qty,
            type=rec.type, symbol=int(symbol),
        )
        out.append(rec)
    native.clear_pending()
    return out


class _AlgoBase:
    """Common Python facade. Subclasses build the native engine
    object in __init__ and route step / fills / observed volume."""

    side: str
    symbol: int

    @property
    def submitted_qty(self) -> float:
        return float(self._native.submitted_qty)

    @property
    def filled_qty(self) -> float:
        return float(self._native.filled_qty)

    @property
    def remaining_qty(self) -> float:
        return float(self._native.remaining_qty)

    def is_done(self) -> bool:
        return bool(self._native.is_done())

    def report_fill(self, qty: float) -> None:
        self._native.report_fill(float(qty))

    def step(self, now_ns: int, executor: Any) -> List[ChildOrder]:
        self._native.step(int(now_ns))
        return _drain_pending(self._native, executor, self.side, self.symbol)


class TWAPExecutor(_AlgoBase):
    def __init__(
        self,
        target_qty: float,
        side: str,
        symbol: int,
        type: str = "market",
        price: float = 0.0,
        duration_ns: int = 0,
        slice_count: int = 0,
        start_time_ns: int = 0,
    ) -> None:
        from flox_py._flox_py import _TWAPExecutorNative  # type: ignore[attr-defined]
        self.target_qty = float(target_qty)
        self.side = _validate_side(side)
        self.symbol = int(symbol)
        self.type = _validate_type(type)
        self.price = float(price)
        self.duration_ns = int(duration_ns)
        self.slice_count = int(slice_count)
        self.start_time_ns = int(start_time_ns)
        self._native = _TWAPExecutorNative(
            target_qty=self.target_qty,
            side=_side_enum(self.side),
            symbol=self.symbol,
            type=_type_enum(self.type),
            limit_price=self.price,
            duration_ns=self.duration_ns,
            slice_count=self.slice_count,
            start_time_ns=self.start_time_ns,
        )

    @property
    def slice_interval_ns(self) -> int:
        return int(self.duration_ns // self.slice_count) if self.slice_count else 0

    @property
    def slice_size(self) -> float:
        return self.target_qty / self.slice_count if self.slice_count else 0.0


class VWAPExecutor(_AlgoBase):
    def __init__(
        self,
        target_qty: float,
        side: str,
        symbol: int,
        type: str = "market",
        price: float = 0.0,
        volume_curve: Optional[Sequence[tuple]] = None,
    ) -> None:
        from flox_py._flox_py import _VWAPExecutorNative  # type: ignore[attr-defined]
        self.target_qty = float(target_qty)
        self.side = _validate_side(side)
        self.symbol = int(symbol)
        self.type = _validate_type(type)
        self.price = float(price)
        self.volume_curve = list(volume_curve or [])
        self._native = _VWAPExecutorNative(
            target_qty=self.target_qty,
            side=_side_enum(self.side),
            symbol=self.symbol,
            type=_type_enum(self.type),
            limit_price=self.price,
            volume_curve=[(int(ts), float(v)) for ts, v in self.volume_curve],
        )


class IcebergExecutor(_AlgoBase):
    def __init__(
        self,
        target_qty: float,
        side: str,
        symbol: int,
        type: str = "market",
        price: float = 0.0,
        visible_qty: float = 0.0,
    ) -> None:
        from flox_py._flox_py import _IcebergExecutorNative  # type: ignore[attr-defined]
        self.target_qty = float(target_qty)
        self.side = _validate_side(side)
        self.symbol = int(symbol)
        self.type = _validate_type(type)
        self.price = float(price)
        self.visible_qty = float(visible_qty)
        self._native = _IcebergExecutorNative(
            target_qty=self.target_qty,
            side=_side_enum(self.side),
            symbol=self.symbol,
            type=_type_enum(self.type),
            limit_price=self.price,
            visible_qty=self.visible_qty,
        )


class POVExecutor(_AlgoBase):
    def __init__(
        self,
        target_qty: float,
        side: str,
        symbol: int,
        type: str = "market",
        price: float = 0.0,
        participation_rate: float = 0.0,
        min_slice_qty: float = 0.0,
    ) -> None:
        from flox_py._flox_py import _POVExecutorNative  # type: ignore[attr-defined]
        self.target_qty = float(target_qty)
        self.side = _validate_side(side)
        self.symbol = int(symbol)
        self.type = _validate_type(type)
        self.price = float(price)
        self.participation_rate = float(participation_rate)
        self.min_slice_qty = float(min_slice_qty)
        self._native = _POVExecutorNative(
            target_qty=self.target_qty,
            side=_side_enum(self.side),
            symbol=self.symbol,
            type=_type_enum(self.type),
            limit_price=self.price,
            participation_rate=self.participation_rate,
            min_slice_qty=self.min_slice_qty,
        )

    def observe_volume(self, qty: float) -> None:
        self._native.observe_volume(float(qty))


__all__ = [
    "ChildOrder",
    "TWAPExecutor",
    "VWAPExecutor",
    "IcebergExecutor",
    "POVExecutor",
]
