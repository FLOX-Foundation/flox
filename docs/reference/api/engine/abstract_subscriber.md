# ISubscriber

`ISubscriber` defines the minimal interface for any component that consumes events via `EventBus`. It provides a stable identity for routing.

```cpp
using SubscriberId = uint64_t;

struct ISubscriber
{
  virtual SubscriberId id() const = 0;
};
```

## Purpose

* Abstract base for all event consumers, enabling uniform routing and identification.

## Responsibilities

| Method | Description                                       |
| ------ | ------------------------------------------------- |
| id()   | Returns a globally unique ID for this subscriber. |

## Notes

* `SubscriberId` is typically derived from pointer identity or hash — no strong ownership implied.
* Uniqueness is required and nothing verifies it. Two subscribers sharing an
  id collide wherever the id is used as a key. For `Strategy` that includes
  order ids, which are numbered within each strategy and prefixed with the
  subscriber id, so two strategies built with the same id emit orders the
  executor and the order tracker cannot tell apart.
* Used by `EventBus<T>` to track subscribers and apply fan-out policy.
* Derived interfaces like `IMarketDataSubscriber` and `IOrderExecutionListener` extend this base.
