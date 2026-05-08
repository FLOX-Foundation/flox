# Wait for multiple feeds with a known staleness budget

Cross-symbol decisions read from N feeds. Without a clock, a strategy that recomputes a ratio on every BTC tick uses a stale ETH price every time the ETH feed lags. The `MultiFeedClock` makes the staleness budget explicit and exposes per-feed lag so the strategy can score its own decisions.

```python
from flox_py.feed_clock import MultiFeedClock, WaitForAll

class PairTrade(Strategy):
    def setup(self):
        self.clock = MultiFeedClock(
            symbols=[btc_id, eth_id],
            policy=WaitForAll,
            timeout_ms=200,
        )

    def on_trade(self, ctx, trade):
        state = self.clock.tick(trade.exchange_ts_ns, ctx.symbol_id)
        if state.fired:
            self.evaluate(state)
```

## Policies

- `WaitForAll`: fire when every listed feed has emitted at least one event since the last fire. If the timeout elapses first, fire anyway with whatever the latest known prices are; `state.staleness_ns` lets the strategy down-weight the decision.
- `FireOnAny`: fire on every tick from any listed feed (the original default). Explicit so the staleness budget is visible to readers.
- `LeaderFollower`: fire on the leader's tick only if every follower's lag is within `staleness_budget_ms`. Used when one feed is the source of truth and the others are confirmation.

## ClockState

`tick()` returns a `ClockState` with:

- `fired`: whether the strategy should evaluate now.
- `last_ts_ns[symbol]`: most recent observed timestamp per feed.
- `staleness_ns[symbol]`: `tick_ts - last_ts` per feed at the moment of fire.
- `triggered_by`: which feed's tick caused the fire (or `None` for out-of-band).

## Out-of-band symbols

Calling `tick()` with a symbol that was not in the original `symbols` list updates the per-symbol last-seen timestamp but never causes a fire on its own. This keeps the clock honest if a strategy is also subscribed to feeds beyond the cross-symbol decision.

## Tests

`python/tests/test_feed_clock.py` covers all three policies, the timeout fallback path, the staleness map, the leader / follower freshness check, and the out-of-band symbol case.
