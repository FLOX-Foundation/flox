# BookUpdateBus

`BookUpdateBus` is a Disruptor-style fan-out event channel for `BookUpdateEvent` messages, wrapped in pooled `Handle`s for zero-allocation delivery across components such as order books, strategies, and analytics.

```cpp
using BookUpdateBus = EventBus<pool::Handle<BookUpdateEvent>>;

std::unique_ptr<BookUpdateBus> createOptimalBookUpdateBus(bool enablePerformanceOptimizations = false);
bool configureBookUpdateBusForPerformance(BookUpdateBus& bus, bool enablePerformanceOptimizations = false);
```

## Purpose

* Efficiently distribute `BookUpdateEvent`s to multiple subscribers with **zero allocations** in the hot path.

## Responsibilities

| Aspect  | Details                                                                 |
| ------- | ----------------------------------------------------------------------- |
| Payload | Uses `pool::Handle<BookUpdateEvent>` for memory reuse and ref-counting. |
| Pattern | Disruptor-style ring buffer with lock-free sequencing.                  |
| Target  | Consumed by order book processors, strategies, and market monitors.     |

## Factory Functions

| Function | Description |
|----------|-------------|
| `createOptimalBookUpdateBus()` | Creates bus with optimal CPU affinity for market data. |
| `configureBookUpdateBusForPerformance()` | Configures existing bus for optimal performance. |

## Notes

* Uses `ComponentType::MARKET_DATA` for CPU affinity configuration.
* Pooling ensures `BookUpdateEvent`s are reused without dynamic heap allocations.
* Designed for high-frequency message flow in HFT environments.
* Supports optional CPU affinity via `FLOX_CPU_AFFINITY_ENABLED`.
