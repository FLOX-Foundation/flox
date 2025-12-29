# CompositeBookMatrix

Thread-safe composite order book across multiple exchanges.

## Header

```cpp
#include "flox/book/composite_book_matrix.h"
```

## Synopsis

```cpp
template <size_t MaxExchanges = 4>
class CompositeBookMatrix : public IMarketDataSubscriber
{
public:
  struct BestQuote {
    int64_t priceRaw{0};
    int64_t qtyRaw{0};
    ExchangeId exchange{InvalidExchangeId};
    bool valid{false};
  };

  // ISubscriber interface
  SubscriberId id() const override;
  void setId(SubscriberId id) noexcept;

  // Writer thread (BookBus consumer)
  void onBookUpdate(const BookUpdateEvent& ev) override;

  // Reader thread (Strategy consumer) - lock-free
  BestQuote bestBid(SymbolId symbol) const noexcept;
  BestQuote bestAsk(SymbolId symbol) const noexcept;

  // Per-exchange queries
  BestQuote bidForExchange(SymbolId symbol, ExchangeId exchange) const noexcept;
  BestQuote askForExchange(SymbolId symbol, ExchangeId exchange) const noexcept;

  // Arbitrage detection
  bool hasArbitrageOpportunity(SymbolId symbol) const noexcept;
  int64_t spreadRaw(SymbolId symbol) const noexcept;

  // Staleness management
  void markStale(ExchangeId exchange, SymbolId symbol) noexcept;
  void markExchangeStale(ExchangeId exchange) noexcept;
  void checkStaleness(int64_t nowNs, int64_t thresholdNs) noexcept;
};
```

## Thread Safety

The CompositeBookMatrix uses atomic operations for thread-safe reads:

- **Writer thread**: `onBookUpdate()` updates atomic top-of-book snapshot
- **Reader threads**: `bestBid()`, `bestAsk()` use lock-free atomic loads

```cpp
// Writer thread (BookBus consumer)
void onBookUpdate(const BookUpdateEvent& ev) {
  // ... find best bid/ask from update ...

  // Atomic publish with release semantics
  exState.bidPrice.store(bestBidPrice, std::memory_order_release);
  exState.bidQty.store(bestBidQty, std::memory_order_release);
  // ...
}

// Reader thread (Strategy consumer)
BestQuote bestBid(SymbolId symbol) const noexcept {
  // Atomic load with acquire semantics
  int64_t price = exState.bidPrice.load(std::memory_order_acquire);
  // ...
}
```

## Usage

### Basic Setup

```cpp
CompositeBookMatrix<4> matrix;  // Support up to 4 exchanges

// Subscribe to BookBus
bookBus.subscribe(&matrix);
```

### Query Best Quotes

```cpp
auto bid = matrix.bestBid(symbol);
auto ask = matrix.bestAsk(symbol);

if (bid.valid && ask.valid) {
  std::cout << "Best bid: " << bid.priceRaw << " on exchange " << bid.exchange << "\n";
  std::cout << "Best ask: " << ask.priceRaw << " on exchange " << ask.exchange << "\n";
  std::cout << "Spread: " << (ask.priceRaw - bid.priceRaw) << "\n";
}
```

### Arbitrage Detection

```cpp
if (matrix.hasArbitrageOpportunity(symbol)) {
  auto bid = matrix.bestBid(symbol);
  auto ask = matrix.bestAsk(symbol);

  // bid.exchange != ask.exchange (different exchanges)
  // bid.priceRaw > ask.priceRaw (can buy low, sell high)

  int64_t profit = bid.priceRaw - ask.priceRaw;
  // Execute arbitrage...
}
```

### Staleness Management

```cpp
// Mark specific symbol on exchange as stale
matrix.markStale(exchangeId, symbol);

// Mark all symbols on exchange as stale (e.g., on disconnect)
matrix.markExchangeStale(exchangeId);

// Periodic staleness check based on time
matrix.checkStaleness(nowNs, staleThresholdNs);
```

## Performance

| Operation | Complexity | Latency |
|-----------|------------|---------|
| onBookUpdate() | O(bids + asks) | ~5ns |
| bestBid() | O(MaxExchanges) | ~3-6ns |
| bestAsk() | O(MaxExchanges) | ~6-10ns |
| hasArbitrageOpportunity() | O(MaxExchanges) | ~11-16ns |
| markStale() | O(1) | <1ns |

## Cache Alignment

The per-exchange state is aligned to 64-byte cache lines to prevent false sharing:

```cpp
struct alignas(64) ExchangeBookState {
  std::atomic<int64_t> bidPrice{0};
  std::atomic<int64_t> bidQty{0};
  std::atomic<int64_t> askPrice{0};
  std::atomic<int64_t> askQty{0};
  std::atomic<int64_t> lastUpdateNs{0};
  std::atomic<bool> stale{true};
};
```

## See Also

- [AggregatedPositionTracker](aggregated_position_tracker.md) - Thread-safe position tracking
- [OrderRouter](order_router.md) - Smart order routing using CompositeBookMatrix
