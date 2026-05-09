# Submit a multi-leg order group

A pair-trade or basket signal needs to send several related orders. The bookkeeping (track each leg's id, query status, unwind on partial fill, cancel the rest on reject) repeats in every strategy that does it. `flox_py.execution.OrderGroup` ships that bookkeeping as a single helper.

```python
from flox_py.execution import OrderGroup, GroupPolicy, GroupStatus

class PairTrade(Strategy):
    def on_bar(self, ctx, bar):
        if not self._signal.fires(bar):
            return
        group = OrderGroup(self, parent_signal_id=signal.id)
        group.add_market_leg(symbol=btc, side=1, qty=0.1)   # short BTC
        group.add_market_leg(symbol=eth, side=0, qty=1.5)   # long ETH
        ids = group.submit()                                # BestEffort by default
        # later, when checking whether to unwind:
        if group.status() == GroupStatus.PARTIALLY_FILLED:
            group.cancel()
```

## Two surfaces — the legacy helper and the engine state machine

There are two `OrderGroup` types in flox today, and they answer different questions:

- `flox_py.execution.OrderGroup` (legacy). Pure-Python helper that submits every leg through the strategy's emit functions and reads back order status; `BestEffort` only. Documented above.
- `flox_py.OrderGroup`. C++ state machine in `include/flox/execution/order_group.h`, exposed through every binding. Records leg events (submit / fill / cancel / failure) and answers two questions: aggregate state, and what actions the strategy *should* take next given the chosen policy. Does no I/O — the strategy wires the recommended actions into its executor.

The state machine is what you use for `AllOrNothing` and `OneSided` semantics. In the cross-binding section below the new surface is the one shown.

## Policies (engine state machine)

- **`BestEffort`**. Submit every leg, observe independently; the group never recommends a corrective action.
- **`AllOrNothing`**. If any leg fails (cancel or rejection), the group enters `Reverting` state and recommends: *cancel* every still-open leg, *revert* every filled leg with an opposite-side market order in the matching quantity.
- **`OneSided`**. Once the first leg fills, the group recommends *cancelling* every remaining open leg.

## Status enum

| Value | Meaning |
|-------|---------|
| `PENDING` | Built but not yet submitted. |
| `SUBMITTED` | All legs emitted; none has reached a terminal state. |
| `PARTIALLY_FILLED` | At least one leg reached a terminal state, others have not. |
| `FILLED` | Every leg reached a terminal state. |
| `CANCELLED` | `cancel()` was called. |
| `REJECTED` | (reserved; engine push hook will populate this) |

The terminal-state check in this Phase 1 helper conservatively treats anything past the `OrderEventStatus::Submitted` family in the engine's enum as "settled" for the purposes of the aggregate. Once the order-event hook lands, the helper switches to a reactive state machine and the status transitions become exact.

## Tests

`python/tests/test_order_group.py` exercises the BestEffort path with a fake strategy that records emitted legs and replays scripted statuses. The fake covers: pending→submitted, partial→filled progression, cancel-only-still-open, double-submit guard, and the `NotImplementedError` raised by the deferred policies.

## Cross-binding (engine state machine)

The same state machine is reachable from every binding. Policies and states are exposed as named string constants — never hand-write integer codes.

```python
# pybind11
import flox_py

g = flox_py.OrderGroup(parent_signal_id=42, policy=flox_py.OrderGroupPolicy.ALL_OR_NOTHING)
g.add_market_leg(symbol=btc, side=0, qty=0.1)
g.add_market_leg(symbol=eth, side=1, qty=2.0)
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
