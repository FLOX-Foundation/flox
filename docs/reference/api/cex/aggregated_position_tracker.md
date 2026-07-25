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
    Quantity quantity{};
    Price avgEntryPrice{};
    Volume costBasis{};
  };

  // Per-exchange position (lock-free read)
  PositionSnapshot position(ExchangeId exchange, SymbolId symbol) const;

  // Aggregated position across all exchanges (lock-free read, O(MaxExchanges))
  PositionSnapshot totalPosition(SymbolId symbol) const;

  // Custom valuator for nonlinear positions (AMM LP, options)
  void setValuator(const IPositionValuator* valuator);

  // Unrealized PnL (lazy, not on hot path)
  Volume unrealizedPnl(SymbolId symbol, Price currentPrice) const;

  // Position update (writer thread only)
  void onFill(ExchangeId exchangeId, SymbolId symbol,
              Quantity filledQty, Price fillPrice);

  // Reset
  void reset(SymbolId symbol);
  void resetAll();
};
```

The public API takes and returns fixed-point types (`Quantity`, `Price`, `Volume`), not raw
`int64_t`. `int64_t` appears only in the private atomic state. `Decimal`'s `int64_t` constructor is
`explicit`, so passing `.raw()` values to these methods does not compile — pass the typed values.

No method is `noexcept`.

## Thread Safety

Reads are lock-free atomic loads; the writer publishes with release semantics.

- **Writer thread**: `onFill()` updates position atomically
- **Reader threads**: `position()`, `totalPosition()` use acquire loads

Per-symbol state lives in a `SymbolStateMap<AtomicPositionState>` per exchange, so `position()` on an
unseen symbol returns a default-constructed snapshot rather than allocating.

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
    Quantity::fromDouble(1.0),
    Price::fromDouble(50000.0));

// Buy 0.5 BTC @ $50,001 on Bybit (exchange 1)
tracker.onFill(1, symbol,
    Quantity::fromDouble(0.5),
    Price::fromDouble(50001.0));

// Sell 0.3 BTC @ $50,002 on Kraken (exchange 2)
// Note: negative quantity for sells
tracker.onFill(2, symbol,
    Quantity::fromDouble(-0.3),
    Price::fromDouble(50002.0));
```

### Query Positions

```cpp
// Per-exchange position (lock-free)
auto binancePos = tracker.position(0, symbol);
std::cout << "Binance: qty=" << binancePos.quantity.toDouble()
          << " avg=" << binancePos.avgEntryPrice.toDouble() << "\n";

// Aggregated position across all exchanges (lock-free)
auto total = tracker.totalPosition(symbol);
std::cout << "Total: qty=" << total.quantity.toDouble()
          << " avg=" << total.avgEntryPrice.toDouble() << "\n";
```

`avgEntryPrice` is derived as `costBasis / quantity` and is left default-constructed when quantity is
zero.

### Unrealized PnL

```cpp
Price currentPrice = Price::fromDouble(50100.0);
Volume pnl = tracker.unrealizedPnl(symbol, currentPrice);
// pnl = totalQty * (currentPrice - avgEntry)
```

### Custom Valuation

`setValuator()` plugs in an `IPositionValuator` for positions whose value is not linear in quantity —
AMM LP positions, options. When a valuator is set it is consulted on every `unrealizedPnl()` call,
including at zero tracked quantity, because such a position derives value from its own state rather
than from a tracked size. With no valuator (the default) PnL is linear.

## Position Math

### Buy Fill
```cpp
cost = cost + (filledQty * fillPrice);
qty  = qty + filledQty;
```

### Sell Fill
```cpp
Quantity sellQty = Quantity::fromRaw(-filledQty.raw());
Price avgEntry = qty.raw() != 0 ? (cost / qty) : Price{};
cost = cost - (sellQty * avgEntry);  // reduce cost at avg entry
qty  = qty - sellQty;
```

### Close to Flat
```cpp
if (qty.raw() == 0) {
  cost = Volume{};  // reset cost basis when flat
}
```

## Performance

| Operation | Complexity |
|-----------|------------|
| `position()` | O(1) |
| `totalPosition()` | O(MaxExchanges) |
| `onFill()` | O(1) |
| `unrealizedPnl()` | O(MaxExchanges) |

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
