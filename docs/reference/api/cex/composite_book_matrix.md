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

## Snapshot vs. delta updates

`onBookUpdate()` treats `BookUpdateEvent::update.type` (`BookUpdateType::SNAPSHOT` or `::DELTA`) differently per side:

- A **SNAPSHOT** replaces both sides of that exchange's top-of-book wholesale. A side with no levels in a snapshot is published as genuinely empty (`bestBid`/`bestAsk`/`bidForExchange`/`askForExchange` report it invalid).
- A **DELTA** only updates the side(s) actually present in the update. A side absent from a delta (a typical incremental frame only carries the side that changed) is left exactly as it was published before -- it is not cleared. If a delta only carries deletions and leaves no live level on a side, that side keeps its last known top rather than reporting a `$0.00` "valid" quote; there is no full order book behind this matrix, so a delta that removes the current top without a replacement level cannot be resolved to a new top until the next snapshot or an update that improves it.

This matters because a real exchange delta commonly touches only one side (e.g. a Bybit `bids`-only incremental frame): before this behaved correctly, any single-sided delta zeroed out the *other* side's price and quantity, making that exchange's whole side disappear from cross-venue comparison until the next snapshot.

## Symbol capacity

Per-symbol state is stored in a fixed `SymbolStateMap<..., 256>` (256 symbols by default). A `SymbolId` at or beyond that capacity has nowhere to live -- the per-exchange state holds atomics, so it cannot use the growable overflow storage available to movable types. Such a write is routed to a shared internal scratch slot instead of the table (visible only in a debug build, which trips an assertion), rather than aliasing symbol 0. If you need more than 256 symbols, instantiate a wider `SymbolStateMap` capacity for your build or shard by exchange group.

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
