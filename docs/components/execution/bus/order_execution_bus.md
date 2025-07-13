# OrderExecutionBus

`OrderExecutionBus` is the delivery channel for `OrderEvent` messages, used to notify downstream components (e.g. PnL trackers, position managers) about order lifecycle events.

```cpp
#ifdef USE_SYNC_ORDER_BUS
using OrderExecutionBus = EventBus<OrderEvent, SyncPolicy<OrderEvent>>;
#else
using OrderExecutionBus = EventBus<OrderEvent, AsyncPolicy<OrderEvent>>;
#endif
```

## Purpose

* Fan-out dispatch of `OrderEvent`s to registered execution listeners with selectable sync/async policy.

## Responsibilities

| Aspect  | Description                                                          |
| ------- | -------------------------------------------------------------------- |
| Payload | Transports `OrderEvent` instances directly (no pooling).             |
| Mode    | Toggled via `USE_SYNC_ORDER_BUS` macro at compile time.              |
| Usage   | Used to notify components like `PositionManager`, `PnLTracker`, etc. |

## Notes

* `SyncPolicy` ensures deterministic propagation — used in simulation/test environments.
* `AsyncPolicy` favors latency and throughput — suitable for production execution.
* Dispatch is resolved via `EventDispatcher<OrderEvent>`, which calls `dispatchTo(listener)`.
