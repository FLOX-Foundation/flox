# BookUpdateEvent

`BookUpdateEvent` represents a snapshot or delta update to the order book, encapsulated in a pooled, memory-resource-aware structure for zero-allocation fan-out.

~~~cpp
struct BookUpdateEvent : public pool::PoolableBase<BookUpdateEvent>
{
  using Listener = IMarketDataSubscriber;

  BookUpdate update;

  int64_t seq{0};      // Exchange sequence number
  int64_t prevSeq{0};  // Previous sequence for gap detection

  uint64_t tickSequence = 0;  // Internal, set by bus

  MonoNanos recvNs{0};       // Local receive time (monotonic)
  MonoNanos publishTsNs{0};  // Bus publish time (monotonic)

  ExchangeId sourceExchange{InvalidExchangeId};  // Source exchange for CEX coordination

  BookUpdateEvent(std::pmr::memory_resource* res);
  void clear();
};
~~~

## Purpose
* Deliver normalized order book changes with minimal latency and no heap allocations.

## Fields

| Field             | Description                                                          |
|-------------------|----------------------------------------------------------------------|
| `update`          | `BookUpdate` payload with bid/ask vectors.                           |
| `seq`             | Exchange-assigned sequence number.                                   |
| `prevSeq`         | Previous sequence number for gap detection.                          |
| `tickSequence`    | Internal bus sequence for ordered delivery.                          |
| `recvNs`          | Local receive timestamp (monotonic nanoseconds).                     |
| `publishTsNs`     | Bus publish timestamp (monotonic nanoseconds).                       |
| `sourceExchange`  | Source exchange ID for CEX coordination.                             |

## Responsibilities

| Aspect        | Details                                                                 |
|---------------|-------------------------------------------------------------------------|
| Memory        | Constructed with `std::pmr::memory_resource` for scoped allocation.     |
| Pooling       | Inherits from `PoolableBase` for reuse via `pool::Handle<T>`.           |
| Payload       | Holds a `BookUpdate` with bid/ask vectors allocated from the PMR.       |
| Sequencing    | `tickSequence` ensures ordered processing across consumers.             |
| Gap Detection | `seq` / `prevSeq` enable detection of missed updates.                   |
| Latency       | `recvNs` / `publishTsNs` enable end-to-end latency measurement.         |
| Subscription  | Declares `IMarketDataSubscriber` as the receiver interface.             |

## Notes
* `clear()` resets bid/ask containers in-place without releasing memory.
* Intended for high-frequency delivery over `BookUpdateBus`.
* Construction asserts non-null memory resource to enforce deterministic allocation control.
* Immutable after dispatch; reused through pooled lifecycle.
* `sourceExchange` enables cross-exchange book aggregation in CEX mode.