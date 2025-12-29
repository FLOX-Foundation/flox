# AggregatedPositionTracker

Thread-safe aggregated position tracking across multiple exchanges.

## Header

```cpp
#include "flox/position/aggregated_position_tracker.h"
```

## Synopsis

```cpp
template <size_t MaxExchanges = 8>
class AggregatedPositionTracker : public ISubsystem
{
public:
  struct PositionSnapshot {
    int64_t quantityRaw{0};
    int64_t avgEntryPriceRaw{0};
    int64_t costBasisRaw{0};
  };

  // Per-exchange position (lock-free read)
  PositionSnapshot position(ExchangeId exchange, SymbolId symbol) const noexcept;

  // Aggregated position across all exchanges (lock-free read, O(MaxExchanges))
  PositionSnapshot totalPosition(SymbolId symbol) const noexcept;

  // Unrealized PnL calculation (lazy, not on hot path)
  int64_t unrealizedPnlRaw(SymbolId symbol, int64_t currentPriceRaw) const noexcept;

  // Position update (writer thread only)
  void onFill(ExchangeId exchangeId, SymbolId symbol,
              int64_t filledQtyRaw, int64_t fillPriceRaw) noexcept;

  // Reset
  void reset(SymbolId symbol) noexcept;
  void resetAll() noexcept;
};
```

## Thread Safety

The AggregatedPositionTracker uses atomic operations for thread-safe reads:

- **Writer thread**: `onFill()` updates position atomically
- **Reader threads**: `position()`, `totalPosition()` use lock-free atomic loads

```cpp
// Writer thread (ExecutionBus consumer)
void onFill(...) {
  // Atomic publish with release semantics
  pos.costBasisRaw.store(cost, std::memory_order_release);
  pos.quantityRaw.store(qty, std::memory_order_release);
}

// Reader thread (Strategy consumer)
PositionSnapshot position(...) const noexcept {
  // Atomic load with acquire semantics
  int64_t qty = state->quantityRaw.load(std::memory_order_acquire);
  int64_t cost = state->costBasisRaw.load(std::memory_order_acquire);
  // ...
}
```

## Usage

### Basic Setup

```cpp
AggregatedPositionTracker<4> tracker;

// Subscribe to ExecutionBus for fill events
// or call onFill() directly
```

### Recording Fills

```cpp
// Buy 1 BTC @ $50,000 on Binance (exchange 0)
tracker.onFill(0, symbol,
    Quantity::fromDouble(1.0).raw(),
    Price::fromDouble(50000.0).raw());

// Buy 0.5 BTC @ $50,001 on Bybit (exchange 1)
tracker.onFill(1, symbol,
    Quantity::fromDouble(0.5).raw(),
    Price::fromDouble(50001.0).raw());

// Sell 0.3 BTC @ $50,002 on Kraken (exchange 2)
// Note: negative quantity for sells
tracker.onFill(2, symbol,
    Quantity::fromDouble(-0.3).raw(),
    Price::fromDouble(50002.0).raw());
```

### Query Positions

```cpp
// Per-exchange position (lock-free)
auto binancePos = tracker.position(0, symbol);
Quantity qty = Quantity::fromRaw(binancePos.quantityRaw);
Price avgEntry = Price::fromRaw(binancePos.avgEntryPriceRaw);
std::cout << "Binance: qty=" << qty.toDouble()
          << " avg=" << avgEntry.toDouble() << "\n";

// Aggregated position across all exchanges (lock-free)
auto total = tracker.totalPosition(symbol);
std::cout << "Total: qty=" << Quantity::fromRaw(total.quantityRaw).toDouble()
          << " avg=" << Price::fromRaw(total.avgEntryPriceRaw).toDouble() << "\n";
```

### Unrealized PnL

```cpp
Price currentPrice = Price::fromDouble(50100.0);
int64_t pnlRaw = tracker.unrealizedPnlRaw(symbol, currentPrice.raw());
double pnl = Price::fromRaw(pnlRaw).toDouble();
// pnl = totalQty * (currentPrice - avgEntry)
```

## Position Math

### Buy Fill
```cpp
cost += fillQty * fillPrice;
qty += fillQty;
```

### Sell Fill
```cpp
avgEntry = (qty != 0) ? cost / qty : 0;
cost -= sellQty * avgEntry;  // Reduce cost at avg entry
qty -= sellQty;
```

### Close to Flat
```cpp
if (qty == 0) {
  cost = 0;  // Reset cost basis when flat
}
```

## Performance

| Operation | Complexity | Latency |
|-----------|------------|---------|
| position() | O(1) | ~3ns |
| totalPosition() | O(MaxExchanges) | ~7ns |
| onFill() | O(1) | ~3ns |
| unrealizedPnlRaw() | O(MaxExchanges) | ~5ns |

## Cache Alignment

The per-position state is aligned to 64-byte cache lines:

```cpp
struct alignas(64) AtomicPositionState {
  std::atomic<int64_t> quantityRaw{0};
  std::atomic<int64_t> costBasisRaw{0};
};
```

## See Also

- [CompositeBookMatrix](composite_book_matrix.md) - Get current prices for PnL calculation
- [SplitOrderTracker](split_order_tracker.md) - Track split orders and their fills
