# CandleBus

`CandleBus` is a Disruptor-style publish-subscribe channel for `CandleEvent` messages, used to deliver aggregated candles from `CandleAggregator` to downstream consumers (e.g., strategies, loggers).

```cpp
using CandleBus = EventBus<CandleEvent>;

std::unique_ptr<CandleBus> createOptimalCandleBus(bool enablePerformanceOptimizations = false);
bool configureCandleBusForPerformance(CandleBus& bus, bool enablePerformanceOptimizations = false);
```

## Purpose

* Fan-out distribution of `CandleEvent` to all registered subscribers.

## Responsibilities

| Aspect  | Details                                                                 |
| ------- | ----------------------------------------------------------------------- |
| Pattern | Disruptor-style ring buffer with lock-free sequencing.                  |
| Binding | Type alias for `EventBus<CandleEvent>`.                                 |
| Usage   | Integrated into `CandleAggregator`; consumed by strategies and metrics. |

## Factory Functions

| Function | Description |
|----------|-------------|
| `createOptimalCandleBus()` | Creates bus with optimal CPU affinity for market data. |
| `configureCandleBusForPerformance()` | Configures existing bus for optimal performance. |

## Notes

* Uses `ComponentType::MARKET_DATA` for CPU affinity configuration.
* Supports optional CPU affinity via `FLOX_CPU_AFFINITY_ENABLED`.
