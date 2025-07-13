# TickGuard

`TickGuard` is a RAII helper that automatically signals completion to a `TickBarrier` when it goes out of scope. It ensures deterministic tick completion even in the presence of early returns or exceptions.

```cpp
class TickGuard {
public:
  explicit TickGuard(TickBarrier& barrier);
  ~TickGuard();

  TickGuard(const TickGuard&) = delete;
  TickGuard& operator=(const TickGuard&) = delete;

private:
  TickBarrier* _barrier;
};
```

## Purpose

* Guarantee that `TickBarrier::complete()` is called once per subscriber at the end of a tick, regardless of control flow.

## Responsibilities

| Behavior     | Description                                     |
| ------------ | ----------------------------------------------- |
| Construction | Stores reference to `TickBarrier`.              |
| Destruction  | Calls `complete()` when the guard is destroyed. |
| Copy/Move    | Explicitly non-copyable and non-movable.        |

## Notes

* Should be declared at the top of the subscriber's `onEvent()` method in `SyncPolicy`.
* Ensures that even early returns or exceptions do not break barrier synchronization.
* No runtime cost beyond pointer storage and destructor call.
