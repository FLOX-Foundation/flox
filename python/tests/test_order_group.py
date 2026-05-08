"""Tests for the multi-leg OrderGroup helper."""
from __future__ import annotations

from typing import Dict, List

import pytest

from flox_py.execution import GroupPolicy, GroupStatus, OrderGroup


class FakeStrategy:
    """Stub that records emitted orders and returns scripted statuses."""

    def __init__(self) -> None:
        self.next_id = 100
        self.market_buys: List[tuple] = []
        self.market_sells: List[tuple] = []
        self.limit_buys: List[tuple] = []
        self.limit_sells: List[tuple] = []
        self.cancels: List[int] = []
        self.statuses: Dict[int, int] = {}

    def emit_market_buy(self, symbol: int, qty: float) -> int:
        oid = self.next_id; self.next_id += 1
        self.market_buys.append((oid, symbol, qty))
        return oid

    def emit_market_sell(self, symbol: int, qty: float) -> int:
        oid = self.next_id; self.next_id += 1
        self.market_sells.append((oid, symbol, qty))
        return oid

    def emit_limit_buy(self, symbol: int, price: float, qty: float) -> int:
        oid = self.next_id; self.next_id += 1
        self.limit_buys.append((oid, symbol, price, qty))
        return oid

    def emit_limit_sell(self, symbol: int, price: float, qty: float) -> int:
        oid = self.next_id; self.next_id += 1
        self.limit_sells.append((oid, symbol, price, qty))
        return oid

    def emit_cancel(self, order_id: int) -> None:
        self.cancels.append(order_id)

    def order_status(self, order_id: int) -> int:
        # 0=submitted, 3=filled (matches Strategy convention used in helper)
        return self.statuses.get(order_id, 0)


def test_pending_until_submitted() -> None:
    s = FakeStrategy()
    g = OrderGroup(s, parent_signal_id=42)
    g.add_market_leg(symbol=1, side=0, qty=0.5)
    assert g.status() == GroupStatus.PENDING


def test_submit_emits_each_leg_and_returns_ids() -> None:
    s = FakeStrategy()
    g = OrderGroup(s, parent_signal_id=42)
    g.add_market_leg(symbol=1, side=0, qty=0.5)
    g.add_market_leg(symbol=2, side=1, qty=2.0)
    g.add_limit_leg(symbol=3, side=0, price=100.0, qty=0.1)
    ids = g.submit()
    assert len(ids) == 3
    assert s.market_buys == [(ids[0], 1, 0.5)]
    assert s.market_sells == [(ids[1], 2, 2.0)]
    assert s.limit_buys == [(ids[2], 3, 100.0, 0.1)]
    assert g.status() == GroupStatus.SUBMITTED


def test_status_progression_partial_then_filled() -> None:
    s = FakeStrategy()
    g = OrderGroup(s)
    g.add_market_leg(symbol=1, side=0, qty=0.1)
    g.add_market_leg(symbol=2, side=1, qty=0.2)
    ids = g.submit()
    s.statuses[ids[0]] = 3  # one filled
    assert g.status() == GroupStatus.PARTIALLY_FILLED
    s.statuses[ids[1]] = 3  # both filled
    assert g.status() == GroupStatus.FILLED


def test_cancel_cancels_open_legs_only() -> None:
    s = FakeStrategy()
    g = OrderGroup(s)
    g.add_market_leg(symbol=1, side=0, qty=0.1)
    g.add_market_leg(symbol=2, side=1, qty=0.2)
    ids = g.submit()
    s.statuses[ids[0]] = 3  # one already filled
    g.cancel()
    # Only the still-active second leg should be cancelled.
    assert s.cancels == [ids[1]]
    assert g.status() == GroupStatus.CANCELLED


def test_unimplemented_policies_raise() -> None:
    s = FakeStrategy()
    g = OrderGroup(s)
    g.add_market_leg(symbol=1, side=0, qty=0.1)
    with pytest.raises(NotImplementedError):
        g.submit(policy=GroupPolicy.ALL_OR_NOTHING)
    with pytest.raises(NotImplementedError):
        g.submit(policy=GroupPolicy.ONE_SIDED)


def test_double_submit_raises() -> None:
    s = FakeStrategy()
    g = OrderGroup(s)
    g.add_market_leg(symbol=1, side=0, qty=0.1)
    g.submit()
    with pytest.raises(RuntimeError):
        g.submit()
