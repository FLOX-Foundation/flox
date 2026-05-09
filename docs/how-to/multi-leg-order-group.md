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

## Follow-ups

- Auto-wire the recommended actions into the executor so strategies don't have to dispatch them by hand. Currently the strategy reads `recommended_actions()` and emits the orders itself.
- Risk hooks at the group level (gross / concentration limits over the legs, not per leg).
- Latency budgets on `OneSided` (only submit leg B if leg A acks within N ms).
