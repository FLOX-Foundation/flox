# BookUpdate

`BookUpdate` is a memory-efficient container for transmitting order book snapshots or deltas, optimized for high-frequency processing via polymorphic memory resources.

```cpp
struct BookUpdate {
  SymbolId symbol{};
  BookUpdateType type{};
  std::pmr::vector<BookLevel> bids;
  std::pmr::vector<BookLevel> asks;
  std::chrono::steady_clock::time_point timestamp{};

  BookUpdate(std::pmr::memory_resource* res);
};
```

## Purpose

* Represent a normalized order book update — either full snapshot or incremental delta — using preallocated memory.

## Responsibilities

| Aspect      | Details                                                                 |
| ----------- | ----------------------------------------------------------------------- |
| Symbol      | `symbol` identifies the instrument the update refers to.                |
| Update Type | `SNAPSHOT` or `DELTA`, used to drive order book reconstruction logic.   |
| Levels      | `bids` and `asks` contain depth updates as `BookLevel` entries.         |
| Timestamp   | Captures the original receive time for sequencing and latency analysis. |
| Allocation  | All storage comes from a user-provided `std::pmr::memory_resource`.     |

## Notes

* Designed for zero-allocation processing when paired with object pools and pre-sized buffers.
* Used in both simulated and real-time environments.
* Consumers must respect the `BookUpdateType` semantics — `SNAPSHOT` implies a full overwrite.
