# Submit a multi-leg order group

A pair-trade or basket signal needs to send several related orders. The bookkeeping (track each leg's id, query status, unwind on partial fill, cancel the rest on reject) repeats in every strategy that does it. `flox_py.OrderGroup` ships that bookkeeping as a single state machine.

The group is a passive state machine: legs + policy + recorded events go in; aggregate state + recommended actions come out. The strategy wires the recommended cancels / reverts into its executor — `OrderGroup` does no I/O itself. The C++ primitive lives in `include/flox/execution/order_group.h` and every binding (pybind11 / NAPI / QuickJS / Codon) wraps the same C ABI, so a strategy in any language gets the same semantics.

```python
import flox_py

g = flox_py.OrderGroup(parent_signal_id=42, policy=flox_py.OrderGroupPolicy.ALL_OR_NOTHING)
g.add_market_leg(symbol=btc, side=0, qty=0.1)   # long BTC
g.add_market_leg(symbol=eth, side=1, qty=2.0)   # short ETH
g.record_submit(0, order_id=100)
g.record_submit(1, order_id=101)
g.record_fill(0, cumulative_qty=0.1)
g.record_failure(1)

for action in g.recommended_actions():
    if action["kind"] == "cancel":
        strategy.emit_cancel(action["order_id"])
    else:  # revert
        if action["side"] == 0:
            strategy.emit_market_buy(action["symbol"], action["qty"])
        else:
            strategy.emit_market_sell(action["symbol"], action["qty"])
```

## Policies

- **`BestEffort`**. Submit every leg, observe independently; the group never recommends a corrective action.
- **`AllOrNothing`**. If any leg fails (cancel or rejection), the group enters `Reverting` state and recommends: *cancel* every still-open leg, *revert* every filled leg with an opposite-side market order in the matching quantity.
- **`OneSided`**. Once the first leg fills, the group recommends *cancelling* every remaining open leg.

Policies and states are exposed as named string constants in JS bindings and as enums (`flox_py.OrderGroupPolicy` / `flox_py.OrderGroupState`) in Python. Hand-writing integer codes is never the right path.

## Cross-binding

```javascript
// node
const { OrderGroup, OrderGroupPolicy } = require('flox');
const g = new OrderGroup({ parentSignalId: 42, policy: OrderGroupPolicy.AllOrNothing });
g.addMarketLeg(btc, 0, 0.1);
g.addMarketLeg(eth, 1, 2.0);
g.recordSubmit(0, 100); g.recordSubmit(1, 101);
g.recordFill(0, 0.1); g.recordFailure(1);
for (const a of g.recommendedActions()) {
  // a.kind: 'cancel' | 'revert'
}
```

```javascript
// QuickJS — `OrderGroup` and `OrderGroupPolicy` are globals from the
// embedded stdlib.
const g = new OrderGroup({ parentSignalId: 42, policy: OrderGroupPolicy.AllOrNothing });
g.addMarketLeg(btc, 0, 0.1);
g.addMarketLeg(eth, 1, 2.0);
```

```python
# codon
from flox.order_group import OrderGroup, ALL_OR_NOTHING

g = OrderGroup(parent_signal_id=42, policy=ALL_OR_NOTHING)
g.add_market_leg(btc, 0, 0.1)
g.add_market_leg(eth, 1, 2.0)
```

## Auto-dispatch through the executor

When the strategy already exposes `emit_cancel(order_id)` /
`emit_market_buy(symbol, qty)` / `emit_market_sell(symbol, qty)`, the
group can fire the recommended actions itself instead of having the
strategy read the list and dispatch by hand:

```python
# pybind11
fired = g.auto_dispatch(strategy)  # returns the number of actions issued
# Calling auto_dispatch again is a no-op — each leg's action is marked
# dispatched once it has been emitted.
```

```javascript
// node
const fired = g.autoDispatch(strategy);
```

```javascript
// QuickJS — the fake strategy must expose .cancel / .marketBuy / .marketSell
const fired = g.autoDispatch(strategy);
```

```python
# codon
fired = g.auto_dispatch(strategy)  # strategy is any class with the three emit_* methods
```

`auto_dispatch` returns the number of actions it fired so the caller can decide whether to react. It is safe to call after every `record_*` event — only newly recommended actions will fire.

## Group-level risk gate

`OrderGroup` accepts a small set of basket-level limits that gate submission before any leg goes out:

| Limit | Meaning | Off when |
|---|---|---|
| `max_gross_notional` | absolute notional sum across legs | zero |
| `max_concentration_pct` | basket gross notional as a fraction of equity | zero |
| `max_leg_qty` | per-leg quantity cap | zero |

`precheck_submission(equity, market_ref_prices)` runs the configured limits against the current legs and returns a breach record. The strategy guards on `breach.denied` before calling `auto_dispatch` or emitting any leg.

```python
# pybind11
g = flox_py.OrderGroup()
g.add_market_leg(symbol=btc, side=0, qty=0.1)
g.add_market_leg(symbol=eth, side=1, qty=2.0)
g.set_risk_limits(max_concentration_pct=0.05)  # cap at 5% of equity
breach = g.precheck_submission(equity=100_000.0,
                                 market_ref_prices=[50_000.0, 3_000.0])
if breach["denied"]:
    log.warning("group denied: %s — %s", breach["rule"], breach["detail"])
    return
g.auto_dispatch(strategy)
```

```javascript
// node
const g = new flox.OrderGroup();
g.addMarketLeg(btc, 0, 0.1);
g.addMarketLeg(eth, 1, 2.0);
g.setRiskLimits({ maxConcentrationPct: 0.05 });
const breach = g.precheckSubmission({
  equity: 100000,
  marketRefPrices: [50000, 3000],
});
if (breach.denied) return;
g.autoDispatch(strategy);
```

```python
# codon
g.set_risk_limits(max_concentration_pct=0.05)
denied, rule, detail = g.precheck_submission(equity=100_000.0,
                                              market_ref_prices=[50_000.0, 3_000.0])
if denied:
    return
g.auto_dispatch(strategy)
```

The cap is **additive** to the per-order risk gates (KillSwitch / OrderValidator / RiskManager) — those still fire on every leg the strategy emits.

## Pair latency budget (`OneSided`)

A pair-trade where the second leg only makes sense once the first one is live needs a deadline. `OrderGroup` exposes a budget the strategy checks each tick to decide whether to submit the follower, cancel the leader on timeout, or keep waiting:

| Decision | When |
|---|---|
| `wait` | budget unset, or leader still inside budget without an ack |
| `submit_follower` | leader acked within the budget |
| `cancel_leader` | leader ack arrived past the budget, or no ack and elapsed time has already exceeded it |

```python
# pybind11
g = flox_py.OrderGroup(policy=flox_py.OrderGroupPolicy.ONE_SIDED)
g.add_limit_leg(btc, 0, 50_000.0, 0.1)
g.add_limit_leg(eth, 1, 3_000.0, 1.5)
g.set_pair_latency_budget_ns(50_000_000)  # 50 ms

# After submitting leg A, on each tick:
decision = g.pair_latency_decision(submit_ts_ns, ack_ts_ns, ack_received=False)
if decision == "submit_follower":
    g.record_submit(1, strategy.emit_limit(...))
elif decision == "cancel_leader":
    strategy.emit_cancel(g.leg_order_id(0))
```

```javascript
// node
const g = new flox.OrderGroup({ policy: 'OneSided' });
g.setPairLatencyBudgetNs(50_000_000);
const d = g.pairLatencyDecision({
  leaderSubmitTsNs: submitTs,
  leaderAckTsNs: nowTs,
  ackReceived: false,
});
```

```javascript
// QuickJS — same shape; `OrderGroup` is a global from the embedded stdlib.
g.setPairLatencyBudgetNs(50_000_000);
const d = g.pairLatencyDecision({ leaderSubmitTsNs, leaderAckTsNs, ackReceived });
```

```python
# codon
g.set_pair_latency_budget_ns(50_000_000)
d = g.pair_latency_decision(submit_ts_ns, ack_ts_ns, False)  # "wait" | "submit_follower" | "cancel_leader"
```

When no ack has arrived yet, pass the current feed time as `leader_ack_ts_ns` — the budget logic treats it as "elapsed since submit" and switches to `cancel_leader` once that exceeds the cap.
