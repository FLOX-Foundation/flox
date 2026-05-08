"""Multi-leg order group helper.

A pair-trade signal (e.g. `signal.symbol_ids = [BTC, ETH]`) needs to
send several related orders. Without a helper every strategy
hand-rolls submission + status tracking + manual unwind. This module
ships an `OrderGroup` that bundles legs under a single
`parent_signal_id` and exposes `submit`, `status`, and `cancel`.

Phase 1 (this module) ships the BestEffort policy: submit every leg,
read status from the strategy's order tracker on demand. AllOrNothing
and OneSided policies need an engine-side hook that pushes
order-event callbacks into Python; that hook is tracked separately.

Example::

    from flox_py.execution import OrderGroup, GroupStatus

    group = OrderGroup(self, parent_signal_id=signal.id)
    group.add_market_leg(symbol=btc, side=0, qty=0.1)
    group.add_market_leg(symbol=eth, side=1, qty=1.5)
    group.submit()
    if group.status() == GroupStatus.FILLED:
        ...
    elif group.status() == GroupStatus.PARTIALLY_FILLED:
        group.cancel()  # cancels remaining legs
"""
from __future__ import annotations

import enum
from dataclasses import dataclass, field
from typing import List, Optional


class GroupStatus(enum.IntEnum):
    PENDING = 0
    SUBMITTED = 1
    PARTIALLY_FILLED = 2
    FILLED = 3
    CANCELLED = 4
    REJECTED = 5


class GroupPolicy(enum.IntEnum):
    BEST_EFFORT = 0   # submit every leg, observe independently
    ALL_OR_NOTHING = 1  # reserved; needs engine-side event hook
    ONE_SIDED = 2       # reserved; same


@dataclass
class _Leg:
    symbol: int
    side: int  # 0 buy, 1 sell
    qty: float
    order_type: str = "market"  # market / limit
    price: Optional[float] = None
    order_id: Optional[int] = None


@dataclass
class OrderGroup:
    strategy: object
    parent_signal_id: int = 0
    legs: List[_Leg] = field(default_factory=list)
    _submitted: bool = False
    _cancelled: bool = False

    def add_market_leg(self, symbol: int, side: int, qty: float) -> int:
        """Add a market-order leg. Returns the leg index for later
        reference."""
        self.legs.append(_Leg(symbol=symbol, side=side, qty=qty,
                              order_type="market"))
        return len(self.legs) - 1

    def add_limit_leg(self, symbol: int, side: int, price: float,
                       qty: float) -> int:
        self.legs.append(_Leg(symbol=symbol, side=side, qty=qty,
                              order_type="limit", price=price))
        return len(self.legs) - 1

    def submit(self, policy: GroupPolicy = GroupPolicy.BEST_EFFORT) -> List[int]:
        """Submit every leg through the strategy's emit helpers.

        Returns the list of order ids assigned by the engine. Only
        BestEffort is implemented; the other policies require an
        engine-side order-event hook that lands in a follow-up task.
        """
        if policy != GroupPolicy.BEST_EFFORT:
            raise NotImplementedError(
                f"OrderGroup policy {policy.name} requires the engine-side "
                "order-event hook that has not landed yet. Use BEST_EFFORT "
                "until the follow-up task closes."
            )
        if self._submitted:
            raise RuntimeError("OrderGroup already submitted")
        self._submitted = True
        ids: List[int] = []
        for leg in self.legs:
            if leg.order_type == "market":
                if leg.side == 0:
                    leg.order_id = self.strategy.emit_market_buy(leg.symbol, leg.qty)
                else:
                    leg.order_id = self.strategy.emit_market_sell(leg.symbol, leg.qty)
            else:  # limit
                if leg.price is None:
                    raise ValueError("limit leg requires price")
                if leg.side == 0:
                    leg.order_id = self.strategy.emit_limit_buy(
                        leg.symbol, leg.price, leg.qty)
                else:
                    leg.order_id = self.strategy.emit_limit_sell(
                        leg.symbol, leg.price, leg.qty)
            ids.append(leg.order_id)
        return ids

    def cancel(self) -> None:
        """Cancel any leg whose order-tracker status is not yet a
        terminal state (filled / cancelled / rejected)."""
        if self._cancelled:
            return
        self._cancelled = True
        for leg in self.legs:
            if leg.order_id is None:
                continue
            try:
                st = self.strategy.order_status(leg.order_id)
            except Exception:
                st = None
            # Status enum values are engine-specific; treat None as
            # "unknown / still active" and try to cancel. The engine
            # is the authority on what's really in flight.
            if st is None or st < 3:  # 3+ are typically terminal in flox enums
                try:
                    self.strategy.emit_cancel(leg.order_id)
                except Exception:
                    pass

    def status(self) -> GroupStatus:
        """Aggregate state across legs derived from order-tracker
        snapshots."""
        if not self._submitted:
            return GroupStatus.PENDING
        if self._cancelled:
            return GroupStatus.CANCELLED
        statuses: List[Optional[int]] = []
        for leg in self.legs:
            if leg.order_id is None:
                statuses.append(None)
                continue
            try:
                statuses.append(self.strategy.order_status(leg.order_id))
            except Exception:
                statuses.append(None)
        # Convention: assume the engine's OrderEventStatus enum has
        # FILLED somewhere in the ack family; we approximate.
        # Without an engine-side hook we just bucket on "any None"
        # vs "all terminal".
        if all(s is not None and s >= 3 for s in statuses):
            return GroupStatus.FILLED
        if any(s is not None and s >= 3 for s in statuses):
            return GroupStatus.PARTIALLY_FILLED
        return GroupStatus.SUBMITTED
