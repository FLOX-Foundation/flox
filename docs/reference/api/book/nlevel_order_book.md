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

## Notes

* Extremely fast and deterministic — suitable for backtests and production.
* Requires external enforcement of tick-aligned prices.
* Offers predictable latency across workloads, assuming sparse updates.
