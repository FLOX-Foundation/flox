# EventDispatcher

`EventDispatcher<T>` provides a compile-time dispatch mechanism that routes events to the appropriate subscriber method without relying on virtual functions or dynamic casting.

```cpp
template <typename T>
struct EventDispatcher;

template <typename T>
struct EventDispatcher<pool::Handle<T>> {
  static void dispatch(const pool::Handle<T>& ev, typename T::Listener& sub);
};

// Specializations for each event type...
```

## Purpose

* Deliver strongly-typed events (`BookUpdateEvent`, `TradeEvent`, etc.) to their matching handler methods on subscribers, using static dispatch.

## Responsibilities

| Specialization    | Routed To                                                     |
| ----------------- | ------------------------------------------------------------- |
| `BookUpdateEvent` | `IMarketDataSubscriber::onBookUpdate()`                       |
| `TradeEvent`      | `IMarketDataSubscriber::onTrade()`                            |
| `CandleEvent`     | `IMarketDataSubscriber::onCandle()`                           |
| `OrderEvent`      | `IOrderExecutionListener::onOrderFilled()` via `dispatchTo()` |
| `pool::Handle<T>` | Unwraps and forwards to `EventDispatcher<T>`                  |

## Notes

* Eliminates virtual overhead in `EventBus` by avoiding `event->dispatchTo()` directly.
* Pooled events (`pool::Handle<T>`) are transparently unwrapped and dispatched.
* Extensible — any new event type must define a matching specialization.
* Dispatch is strictly type-safe and resolved at compile time.
