"""Tests for the OrderGroup state machine.

`flox_py.OrderGroup` is bound from `include/flox/execution/order_group.h`
and is the same primitive every binding (pybind11 / NAPI / QuickJS /
Codon) reaches through the C ABI.
"""
from __future__ import annotations

import pytest

import flox_py


def test_best_effort_recommends_nothing() -> None:
    g = flox_py.OrderGroup(parent_signal_id=1, policy=flox_py.OrderGroupPolicy.BEST_EFFORT)
    a = g.add_market_leg(symbol=1, side=0, qty=0.1)
    b = g.add_market_leg(symbol=2, side=1, qty=2.0)
    assert g.leg_count() == 2
    assert g.state() == flox_py.OrderGroupState.PENDING
    g.record_submit(a, 100)
    g.record_submit(b, 101)
    assert g.state() == flox_py.OrderGroupState.SUBMITTED
    g.record_fill(a, 0.1)
    g.record_failure(b)
    assert g.state() == flox_py.OrderGroupState.PARTIALLY_FILLED
    assert g.recommended_actions() == []


def test_one_sided_cancels_remaining_on_first_fill() -> None:
    g = flox_py.OrderGroup(policy=flox_py.OrderGroupPolicy.ONE_SIDED)
    g.add_limit_leg(symbol=1, side=0, price=50000.0, qty=0.1)
    g.add_limit_leg(symbol=2, side=1, price=3000.0, qty=1.5)
    g.record_submit(0, 200)
    g.record_submit(1, 201)
    g.record_fill(0, 0.1)

    actions = g.recommended_actions()
    assert len(actions) == 1
    assert actions[0]["kind"] == "cancel"
    assert actions[0]["leg_index"] == 1
    assert actions[0]["order_id"] == 201


def test_all_or_nothing_reverts_filled_legs_on_failure() -> None:
    g = flox_py.OrderGroup(policy=flox_py.OrderGroupPolicy.ALL_OR_NOTHING)
    g.add_market_leg(1, 0, 0.1)
    g.add_market_leg(2, 1, 2.0)
    g.record_submit(0, 300)
    g.record_submit(1, 301)
    g.record_fill(0, 0.1)
    g.record_failure(1)

    assert g.state() == flox_py.OrderGroupState.REVERTING
    actions = g.recommended_actions()
    assert len(actions) == 1
    assert actions[0]["kind"] == "revert"
    assert actions[0]["symbol"] == 1
    # Opposite side of the original BUY (0) → SELL (1)
    assert actions[0]["side"] == 1
    assert actions[0]["qty"] == pytest.approx(0.1)


def test_auto_dispatch_fires_actions_through_strategy() -> None:
    """T005 — `auto_dispatch(strategy)` walks the recommended actions
    and emits the matching cancel / revert calls through the strategy.
    A second call is a no-op because each action is marked
    dispatched."""

    class FakeStrat:
        def __init__(self) -> None:
            self.cancels: list = []
            self.market_buys: list = []
            self.market_sells: list = []

        def emit_cancel(self, order_id: int) -> None:
            self.cancels.append(order_id)

        def emit_market_buy(self, symbol: int, qty: float) -> None:
            self.market_buys.append((symbol, qty))

        def emit_market_sell(self, symbol: int, qty: float) -> None:
            self.market_sells.append((symbol, qty))

    s = FakeStrat()
    g = flox_py.OrderGroup(policy=flox_py.OrderGroupPolicy.ALL_OR_NOTHING)
    g.add_market_leg(symbol=1, side=0, qty=0.1)  # buy BTC
    g.add_market_leg(symbol=2, side=1, qty=2.0)  # sell ETH
    g.record_submit(0, 100)
    g.record_submit(1, 101)
    g.record_fill(0, 0.1)
    g.record_failure(1)

    fired = g.auto_dispatch(s)
    assert fired == 1
    # Filled BTC buy → revert as a market sell.
    assert s.market_sells == [(1, pytest.approx(0.1))]
    assert s.market_buys == []
    assert s.cancels == []
    # Idempotent — no double-fire.
    assert g.auto_dispatch(s) == 0


def test_risk_gate_denies_basket_over_concentration_cap() -> None:
    """Group-level risk gate: basket gross notional exceeding the
    concentration cap (% of equity) must deny submission."""
    g = flox_py.OrderGroup()
    g.add_market_leg(symbol=1, side=0, qty=0.1)  # BTC
    g.add_market_leg(symbol=2, side=1, qty=2.0)  # ETH
    g.set_risk_limits(max_concentration_pct=0.05)

    # Equity 100k; leg notionals 0.1 * 50k + 2.0 * 3k = 11k → 11% > 5%.
    breach = g.precheck_submission(equity=100_000.0,
                                    market_ref_prices=[50_000.0, 3_000.0])
    assert breach["denied"] is True
    assert breach["rule"] == "maxConcentrationPct"

    # Smaller basket: 0.001 * 50k + 0.001 * 3k = 53 → well under 5%.
    g2 = flox_py.OrderGroup()
    g2.add_market_leg(symbol=1, side=0, qty=0.001)
    g2.add_market_leg(symbol=2, side=1, qty=0.001)
    g2.set_risk_limits(max_concentration_pct=0.05)
    assert g2.precheck_submission(equity=100_000.0,
                                   market_ref_prices=[50_000.0, 3_000.0])["denied"] is False


def test_risk_gate_per_leg_cap_denies_oversized_leg() -> None:
    g = flox_py.OrderGroup()
    g.add_market_leg(symbol=1, side=0, qty=10.0)
    g.set_risk_limits(max_leg_qty=1.0)
    breach = g.precheck_submission()
    assert breach["denied"] is True
    assert breach["rule"] == "maxLegQty"


def test_auto_dispatch_one_sided_cancels_remaining_legs() -> None:
    class FakeStrat:
        def __init__(self) -> None:
            self.cancels: list = []
            self.market_buys: list = []
            self.market_sells: list = []

        def emit_cancel(self, order_id: int) -> None:
            self.cancels.append(order_id)

        def emit_market_buy(self, symbol: int, qty: float) -> None:
            self.market_buys.append((symbol, qty))

        def emit_market_sell(self, symbol: int, qty: float) -> None:
            self.market_sells.append((symbol, qty))

    s = FakeStrat()
    g = flox_py.OrderGroup(policy=flox_py.OrderGroupPolicy.ONE_SIDED)
    g.add_limit_leg(symbol=1, side=0, price=50000.0, qty=0.1)
    g.add_limit_leg(symbol=2, side=1, price=3000.0, qty=1.5)
    g.record_submit(0, 200)
    g.record_submit(1, 201)
    g.record_fill(0, 0.1)
    fired = g.auto_dispatch(s)
    assert fired == 1
    assert s.cancels == [201]


def test_pair_latency_decision_returns_wait_when_unset() -> None:
    g = flox_py.OrderGroup(policy=flox_py.OrderGroupPolicy.ONE_SIDED)
    g.add_limit_leg(symbol=1, side=0, price=50000.0, qty=0.1)
    g.add_limit_leg(symbol=2, side=1, price=3000.0, qty=1.5)
    assert g.pair_latency_decision(0, 0, False) == "wait"


def test_pair_latency_decision_paths() -> None:
    g = flox_py.OrderGroup(policy=flox_py.OrderGroupPolicy.ONE_SIDED)
    g.add_limit_leg(symbol=1, side=0, price=50000.0, qty=0.1)
    g.add_limit_leg(symbol=2, side=1, price=3000.0, qty=1.5)
    g.set_pair_latency_budget_ns(50_000_000)  # 50 ms

    submit_ts = 1_000_000_000
    # Ack within budget → submit follower.
    assert g.pair_latency_decision(submit_ts, submit_ts + 30_000_000, True) == "submit_follower"
    # Ack over budget → cancel leader.
    assert g.pair_latency_decision(submit_ts, submit_ts + 60_000_000, True) == "cancel_leader"
    # No ack yet, still inside budget → keep waiting.
    assert g.pair_latency_decision(submit_ts, submit_ts + 10_000_000, False) == "wait"
    # No ack yet, past budget → cancel leader on timeout.
    assert g.pair_latency_decision(submit_ts, submit_ts + 80_000_000, False) == "cancel_leader"


def test_partial_fill_marks_leg_partially_filled() -> None:
    g = flox_py.OrderGroup(policy=flox_py.OrderGroupPolicy.BEST_EFFORT)
    g.add_market_leg(1, 0, 0.5)
    g.record_submit(0, 400)
    g.record_fill(0, 0.2)  # half of target
    assert g.leg_state(0) == flox_py.LegState.PARTIALLY_FILLED
    g.record_fill(0, 0.5)
    assert g.leg_state(0) == flox_py.LegState.FILLED
