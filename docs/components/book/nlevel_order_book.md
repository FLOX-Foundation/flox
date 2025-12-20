# NLevelOrderBook

`NLevelOrderBook` is a high-performance, fixed-depth limit order book optimized for HFT and simulation. It uses tick-based indexing for fast access and zero allocations in the hot path.

```cpp
template <size_t MaxLevels = 8192>
class NLevelOrderBook : public IOrderBook {
  // ...
};
```

## Purpose

* Maintain and query an efficient in-memory representation of top-of-book and full depth using indexed price levels.

## Responsibilities

| Aspect      | Details                                                                    |
| ----------- | -------------------------------------------------------------------------- |
| Input       | Consumes `BookUpdateEvent` messages, supports both `SNAPSHOT` and `DELTA`. |
| Resolution  | Tick-based price quantization via `_tickSize`.                             |
| Depth Query | Provides `bestBid`, `bestAsk`, and `Quantity` at arbitrary price levels.   |
| Storage     | Preallocated arrays for bids and asks indexed by tick-level offset.        |

## Internal Behavior

1. **Price Indexing**
   Prices are mapped to array indices using `price / tickSize`, enabling constant-time access.

2. **Snapshot Handling**
   A `SNAPSHOT` clears all state and resets index bounds before applying levels.

3. **Bounds Tracking**
   Maintains `_minBidIndex`, `_maxBidIndex`, `_minAskIndex`, `_maxAskIndex` for efficient best-level scans.

4. **Best Bid/Ask Scan**
   Performs linear scans within index bounds to locate top of book — fast due to tight range.

5. **No Dynamic Allocation**
   Uses `std::array` of fixed size; fully cache-friendly and allocation-free after construction.

## Market State Helpers

```cpp
[[nodiscard]] bool isCrossed() const noexcept;
[[nodiscard]] std::optional<Price> spread() const noexcept;
[[nodiscard]] std::optional<Price> mid() const noexcept;
```

| Method      | Description                                                      |
| ----------- | ---------------------------------------------------------------- |
| `isCrossed` | Returns `true` if best bid >= best ask (crossed/locked market).  |
| `spread`    | Returns ask - bid spread, or `nullopt` if either side is empty.  |
| `mid`       | Returns midpoint price, or `nullopt` if either side is empty.    |

These methods are useful for detecting market anomalies and calculating fair value:

```cpp
if (book.isCrossed()) {
  // Handle crossed book condition
}

if (auto mid = book.mid()) {
  // Use midpoint for fair value calculations
}
```

## Notes

* Extremely fast and deterministic — suitable for backtests and production.
* Requires external enforcement of tick-aligned prices.
* Offers predictable latency across workloads, assuming sparse updates.
