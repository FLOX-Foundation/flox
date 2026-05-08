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

## Policies

Three policies are listed in the API; only `BestEffort` is implemented today:

- **`BEST_EFFORT`** (default). Submit every leg through the strategy's emit helpers. The user polls `status()` to see what's been filled. `cancel()` cancels still-open legs.
- **`ALL_OR_NOTHING`**. Reactive: if any leg rejects, cancel the rest; if any leg partially fills while another rejects, unwind the filled portion. Needs an engine-side push hook for order events that has not landed yet — calling `submit(policy=ALL_OR_NOTHING)` raises `NotImplementedError` until the follow-up task closes.
- **`ONE_SIDED`**. Submit leg A first; only submit leg B if A acks within a latency budget. Same engine-hook prerequisite as above.

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

## Follow-ups

- Engine-side order-event hook into Python / Node / Codon / QuickJS strategies. Once landed, AllOrNothing and OneSided fold in cleanly.
- Risk hooks at the group level (gross / concentration limits over the legs, not per leg).
- Polyglot parity for NAPI / Codon / QuickJS once the engine hook lands.
