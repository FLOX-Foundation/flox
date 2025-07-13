# CandleBus

`CandleBus` is a publish-subscribe channel for `CandleEvent` messages, used to deliver aggregated candles from `CandleAggregator` to downstream consumers (e.g., strategies, loggers).

```cpp
#ifdef USE_SYNC_CANDLE_BUS
using CandleBus = EventBus<CandleEvent, SyncPolicy<CandleEvent>>;
#else
using CandleBus = EventBus<CandleEvent, AsyncPolicy<CandleEvent>>;
#endif
```

## Purpose

* Fan-out distribution of `CandleEvent` to all registered subscribers.

## Responsibilities

| Aspect  | Details                                                                 |
| ------- | ----------------------------------------------------------------------- |
| Policy  | Chooses between `SyncPolicy` and `AsyncPolicy` via compile-time flag.   |
| Binding | Type alias for `EventBus<CandleEvent, Policy>`.                         |
| Usage   | Integrated into `CandleAggregator`; consumed by strategies and metrics. |

## Notes

* `SyncPolicy` ensures deterministic tick-to-tick sequencing for backtesting.
* `AsyncPolicy` enables low-latency lock-free fan-out in live trading.
* Controlled via the `USE_SYNC_CANDLE_BUS` macro, toggled at compile time.
