# OrderExecutionBus

`OrderExecutionBus` is a Disruptor-style delivery channel for `OrderEvent` messages, used to notify downstream components (e.g. PnL trackers, position managers) about order lifecycle events.

```cpp
using OrderExecutionBus = EventBus<OrderEvent>;

std::unique_ptr<OrderExecutionBus> createOptimalOrderExecutionBus(bool enablePerformanceOptimizations = false);
bool configureOrderExecutionBusForPerformance(OrderExecutionBus& bus, bool enablePerformanceOptimizations = false);
```

## Purpose

* Fan-out dispatch of `OrderEvent`s to registered execution listeners.

## Responsibilities

| Aspect  | Description                                                          |
| ------- | -------------------------------------------------------------------- |
| Payload | Transports `OrderEvent` instances directly (no pooling).             |
| Pattern | Disruptor-style ring buffer with lock-free sequencing.               |
| Usage   | Used to notify components like `PositionManager`, `PnLTracker`, etc. |

## Factory Functions

| Function | Description |
|----------|-------------|
| `createOptimalOrderExecutionBus()` | Creates bus with optimal CPU affinity for execution. |
| `configureOrderExecutionBusForPerformance()` | Configures existing bus for optimal performance. |

## Notes

* Uses `ComponentType::EXECUTION` for CPU affinity configuration.
* Dispatch is resolved via `EventDispatcher<OrderEvent>`, which calls `dispatchTo(listener)`.
* Supports optional CPU affinity via `FLOX_CPU_AFFINITY_ENABLED`.
