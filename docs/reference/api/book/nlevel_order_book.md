# NLevelOrderBook

`NLevelOrderBook` is a high-performance, fixed-depth limit order book optimized for HFT and simulation. It uses tick-based indexing for fast access and zero allocations in the hot path.

```cpp
template <size_t MaxLevels = 8192>
class NLevelOrderBook : public IOrderBook
{
public:
  static constexpr size_t MAX_LEVELS = MaxLevels;

  explicit NLevelOrderBook(Price tickSize);

  void applyBookUpdate(const BookUpdateEvent& ev) override;

  std::optional<Price> bestBid() const override;
  std::optional<Price> bestAsk() const override;
  Quantity bidAtPrice(Price p) const override;
  Quantity askAtPrice(Price p) const override;

  // Market state helpers
  bool isCrossed() const;
  std::optional<Price> spread() const;
  std::optional<Price> mid() const;

  // Depth consumption (slippage calculation)
  std::pair<Quantity, Volume> consumeAsks(Quantity needQty) const;
  std::pair<Quantity, Volume> consumeBids(Quantity needQty) const;

  // Level extraction
  struct PriceLevel { Price price; Quantity quantity; };
  std::vector<PriceLevel> getBidLevels(size_t maxLevels) const;
  std::vector<PriceLevel> getAskLevels(size_t maxLevels) const;

  // Utilities
  Price tickSize() const;
  void clear();
  void dump(std::ostream& os, size_t levels,
            int pricePrec = 4, int qtyPrec = 3, bool ansi = false) const;
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
   Maintains `_minBid`, `_maxBid`, `_minAsk`, `_maxAsk` for efficient best-level scans.

4. **Cached Best Bid/Ask**
   Tracks `_bestBidTick` and `_bestAskTick` for O(1) best price queries without scanning.

5. **No Dynamic Allocation**
   Uses `std::array` of fixed size; fully cache-friendly and allocation-free after construction.

6. **Cache-Aligned Storage**
   Bids and asks arrays are 64-byte aligned for optimal cache performance.

## Market State Helpers

```cpp
bool isCrossed() const;
std::optional<Price> spread() const;
std::optional<Price> mid() const;
```

| Method      | Description                                                      |
| ----------- | ---------------------------------------------------------------- |
| `isCrossed` | Returns `true` if best bid >= best ask (crossed/locked market).  |
| `spread`    | Returns ask - bid spread, or `nullopt` if either side is empty.  |
| `mid`       | Returns midpoint price, or `nullopt` if either side is empty.    |

## Depth Consumption

Calculate fill price and slippage by walking through book levels:

```cpp
// Simulate market buy: consume asks up to 10 BTC
auto [filledQty, totalCost] = book.consumeAsks(Quantity::fromDouble(10.0));
Price avgPrice = totalCost / filledQty;

// Simulate market sell: consume bids
auto [filledQty, totalProceeds] = book.consumeBids(Quantity::fromDouble(10.0));
```

Uses 128-bit arithmetic on GCC/Clang for precision; portable fallback otherwise.

## Level Extraction

Get multiple price levels for display or analysis:

```cpp
// Get top 10 bid levels
auto bids = book.getBidLevels(10);
for (const auto& level : bids) {
  std::cout << level.price.toDouble() << " @ " << level.quantity.toDouble() << "\n";
}

// Get top 10 ask levels
auto asks = book.getAskLevels(10);
```

## Debug Output

```cpp
// Print order book to console with ANSI colors
book.dump(std::cout, 20, /*pricePrec=*/2, /*qtyPrec=*/4, /*ansi=*/true);
```

Output includes tick size, base index, spread, and mid price in header.

## Notes

* Extremely fast and deterministic — suitable for backtests and production.
* Requires external enforcement of tick-aligned prices.
* Offers predictable latency across workloads, assuming sparse updates.
* Uses `math::FastDiv64` for optimized tick division.
