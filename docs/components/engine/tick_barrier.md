# TickBarrier

`TickBarrier` is a lightweight synchronization primitive used to coordinate a fixed number of consumers during synchronous tick processing. It enables deterministic progression to the next event only after all subscribers have completed processing.

```cpp
class TickBarrier {
public:
  explicit TickBarrier(size_t total);

  void complete();
  void wait();

private:
  const size_t _total;
  std::atomic<size_t> _completed;
};
```

## Purpose

* Synchronize fan-out consumers in `SyncPolicy` to ensure all subscribers complete before the next tick is published.

## Responsibilities

| Method   | Description                                                   |
| -------- | ------------------------------------------------------------- |
| complete | Called by each subscriber when it finishes processing a tick. |
| wait     | Blocks until all expected completions have been received.     |

## Notes

* Spin-waiting via `std::this_thread::yield()` — no locks, no condition variables.
* Suitable only for short-lived, low-latency operations (e.g., simulation/batch replay).
* Initialized with `total` — the number of expected `complete()` calls per tick.
* Not reusable — one barrier per tick. Resetting requires re-instantiation.
