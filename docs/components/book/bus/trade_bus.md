# TradeBus

`TradeBus` is a high-throughput Disruptor-style delivery channel for `TradeEvent` messages, used to broadcast trade prints across system components such as aggregators, strategies, and analytics modules.

```cpp
using TradeBus = EventBus<TradeEvent>;

std::unique_ptr<TradeBus> createOptimalTradeBus(bool enablePerformanceOptimizations = false);
bool configureTradeBusForPerformance(TradeBus& bus, bool enablePerformanceOptimizations = false);
```

## Purpose

* Propagate real-time `TradeEvent`s to all registered consumers in the system.

## Responsibilities

| Aspect   | Details                                                                 |
|----------|-------------------------------------------------------------------------|
| Payload  | Direct delivery of `TradeEvent` instances (no wrapping or pooling).     |
| Pattern  | Disruptor-style ring buffer with lock-free sequencing.                  |
| Usage    | Used by connectors, aggregators (e.g., `CandleAggregator`), and strategies.|

## Factory Functions

| Function | Description |
|----------|-------------|
| `createOptimalTradeBus()` | Creates bus with optimal CPU affinity for market data. |
| `configureTradeBusForPerformance()` | Configures existing bus for optimal performance. |

## Notes

* Uses `ComponentType::MARKET_DATA` for CPU affinity configuration.
* Stateless; the bus itself performs no buffering or transformation.
* Supports optional CPU affinity via `FLOX_CPU_AFFINITY_ENABLED`.
