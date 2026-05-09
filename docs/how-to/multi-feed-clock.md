# Wait for multiple feeds with a known staleness budget

Cross-symbol decisions read from N feeds. Without a clock, a strategy that recomputes a ratio on every BTC tick uses a stale ETH price every time the ETH feed lags. The `MultiFeedClock` makes the staleness budget explicit and exposes per-feed lag so the strategy can score its own decisions.

The clock lives in `include/flox/feed/multi_feed_clock.h` as a C++ primitive. Every binding (pybind11 / NAPI / QuickJS / Codon) wraps the same C ABI so a strategy in any language gets the same semantics and the same hot-path cost. Policies are exposed as named string constants in JS bindings and as an enum (`flox_py.FeedClockPolicy`) in Python — users never hand-write integer codes.

```python
# pybind11
import flox_py

clock = flox_py.MultiFeedClock(
    symbols=[btc, eth],
    policy=flox_py.FeedClockPolicy.WAIT_FOR_ALL,
    timeout_ms=200,
)

class PairTrade(Strategy):
    def on_trade(self, ctx, trade):
        state = clock.tick(trade.exchange_ts_ns, ctx.symbol_id)
        if state["fired"]:
            self.evaluate(state)
```

## Policies

- `WaitForAll`: fire when every listed feed has emitted at least one event since the last fire. If the timeout elapses first, fire anyway with whatever the latest known prices are; the per-feed `staleness_ns` map lets the strategy down-weight the decision.
- `FireOnAny`: fire on every tick from any listed feed. Explicit so the staleness budget is visible to readers.
- `LeaderFollower`: fire on the leader's tick only if every follower's lag is within `staleness_budget_ms`. Used when one feed is the source of truth and the others are confirmation.

## Tick result

`tick(ts_ns, symbol)` returns a snapshot with:

- `fired`: whether the strategy should evaluate now.
- `last_ts_ns[symbol]`: most recent observed timestamp per feed.
- `staleness_ns[symbol]`: `tick_ts - last_ts` per feed at the moment of fire.
- `triggered_by`: which feed's tick caused the fire (or `0` for out-of-band).

## Out-of-band symbols

Calling `tick()` with a symbol that was not in the original `symbols` list updates the per-symbol last-seen timestamp but never causes a fire on its own. This keeps the clock honest if a strategy is also subscribed to feeds beyond the cross-symbol decision.

## Cross-binding

```javascript
// node
const { MultiFeedClock, FeedClockPolicy } = require('flox');
const clock = new MultiFeedClock({
  symbols: [btc, eth],
  policy: FeedClockPolicy.WaitForAll,
  timeoutMs: 200,
});
const state = clock.tick(tsNs, btc);
```

```javascript
// QuickJS — `MultiFeedClock` and `FeedClockPolicy` are globals
const clock = new MultiFeedClock({
  symbols: [btc, eth],
  policy: FeedClockPolicy.LeaderFollower,
  leaderSymbol: btc,
  stalenessBudgetMs: 200,
});
```

```python
# codon
from flox.feed_clock import MultiFeedClock, WAIT_FOR_ALL
clock = MultiFeedClock(symbols=[btc, eth], policy=WAIT_FOR_ALL, timeout_ms=200)
state = clock.tick(ts_ns, btc)
```

## Tests

`python/tests/test_feed_clock.py` covers all three policies, the timeout fallback path, the staleness map, the leader / follower freshness check.
