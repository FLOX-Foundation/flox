# TradeBus

`TradeBus` is a high-throughput delivery channel for `TradeEvent` messages, used to broadcast trade prints across system components such as aggregators, strategies, and analytics modules.

~~~cpp
#ifdef USE_SYNC_MARKET_BUS
using TradeBus = EventBus<TradeEvent, SyncPolicy<TradeEvent>>;
#else
using TradeBus = EventBus<TradeEvent, AsyncPolicy<TradeEvent>>;
#endif
~~~

## Purpose
* Propagate real-time `TradeEvent`s to all registered consumers in the system.

## Responsibilities

| Aspect   | Details                                                                 |
|----------|-------------------------------------------------------------------------|
| Payload  | Direct delivery of `TradeEvent` instances (no wrapping or pooling).     |
| Mode     | Chooses between `SyncPolicy` and `AsyncPolicy` via compile-time macro. |
| Usage    | Used by connectors, aggregators (e.g., `CandleAggregator`), and strategies.|

## Notes
* `SyncPolicy` guarantees deterministic tick-to-tick replay for simulation/backtesting.
* `AsyncPolicy` offers lock-free fan-out for live environments with minimal latency.
* Stateless; the bus itself performs no buffering or transformation.