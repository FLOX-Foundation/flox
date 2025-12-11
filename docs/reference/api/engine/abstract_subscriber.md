# ISubscriber

`ISubscriber` defines the minimal interface for any component that consumes events via `EventBus`. It provides a stable identity and declares the delivery mode (push or pull).

```cpp
struct ISubscriber {
  virtual SubscriberId id() const = 0;
  virtual SubscriberMode mode() const { return SubscriberMode::PUSH; }
};

using SubscriberId = uint64_t;
enum class SubscriberMode {
  PUSH,
  PULL
};
```

## Purpose

* Abstract base for all event consumers, enabling uniform routing and delivery control.

## Responsibilities

| Method | Description                                                        |
| ------ | ------------------------------------------------------------------ |
| id()   | Returns a globally unique ID for this subscriber.                  |
| mode() | Declares delivery mode: `PUSH` (default) or `PULL` (manual drain). |

## Notes

* `PUSH` mode: the bus invokes the subscriber immediately on event dispatch.
* `PULL` mode: the subscriber drains from a queue explicitly (e.g., with batching).
* `SubscriberId` is typically derived from pointer identity or hash — no strong ownership implied.
* Used by `EventBus<T>` to track subscribers and apply fan-out policy.
