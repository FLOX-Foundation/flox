# BookUpdateBus

`BookUpdateBus` is a fan-out event channel for `BookUpdateEvent` messages, wrapped in pooled `Handle`s for zero-allocation delivery across components such as order books, strategies, and analytics.

```cpp
#ifdef USE_SYNC_MARKET_BUS
using BookUpdateBus = EventBus<pool::Handle<BookUpdateEvent>, SyncPolicy<...>>;
#else
using BookUpdateBus = EventBus<pool::Handle<BookUpdateEvent>, AsyncPolicy<...>>;
#endif
```

## Purpose

* Efficiently distribute `BookUpdateEvent`s to multiple subscribers with **zero allocations** in the hot path.

## Responsibilities

| Aspect  | Details                                                                 |
| ------- | ----------------------------------------------------------------------- |
| Payload | Uses `pool::Handle<BookUpdateEvent>` for memory reuse and ref-counting. |
| Mode    | `SyncPolicy` or `AsyncPolicy`, toggled by `USE_SYNC_MARKET_BUS`.        |
| Target  | Consumed by order book processors, strategies, and market monitors.     |

## Notes

* `SyncPolicy` enforces barrier-based delivery (e.g., for simulation or determinism).
* `AsyncPolicy` supports lock-free fan-out under production latency constraints.
* Pooling ensures `BookUpdateEvent`s are reused without dynamic heap allocations.
* Designed for high-frequency message flow in HFT environments.
