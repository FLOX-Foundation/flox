# BookUpdateEvent

`BookUpdateEvent` represents a snapshot or delta update to the order book, encapsulated in a pooled, memory-resource-aware structure for zero-allocation fan-out.

~~~cpp
struct BookUpdateEvent : public pool::PoolableBase<BookUpdateEvent> {
  using Listener = IMarketDataSubscriber;

  BookUpdate update;
  uint64_t tickSequence = 0;

  BookUpdateEvent(std::pmr::memory_resource* res);
  void clear();
};
~~~

## Purpose
* Deliver normalized order book changes with minimal latency and no heap allocations.

## Responsibilities

| Aspect        | Details                                                                 |
|---------------|-------------------------------------------------------------------------|
| Memory        | Constructed with `std::pmr::memory_resource` for scoped allocation.     |
| Pooling       | Inherits from `PoolableBase` for reuse via `pool::Handle<T>`.           |
| Payload       | Holds a `BookUpdate` with bid/ask vectors allocated from the PMR.       |
| Sequencing    | `tickSequence` ensures ordered processing across consumers.             |
| Subscription  | Declares `IMarketDataSubscriber` as the receiver interface.             |

## Notes
* `clear()` resets bid/ask containers in-place without releasing memory.
* Intended for high-frequency delivery over `BookUpdateBus`.
* Construction asserts non-null memory resource to enforce deterministic allocation control.
* Immutable after dispatch; reused through pooled lifecycle.