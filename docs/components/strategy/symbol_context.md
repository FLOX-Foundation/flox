# SymbolContext

`SymbolContext` aggregates all per-symbol state needed by trading strategies: order book, position, prices, and timestamps.

```cpp
struct SymbolContext
{
  static constexpr size_t kDefaultBookLevels = 512;

  NLevelOrderBook<kDefaultBookLevels> book;
  Quantity position{};
  Price avgEntryPrice{};
  Price lastTradePrice{};
  int64_t lastUpdateNs{0};
  SymbolId symbolId{0};
};
```

## Purpose

- Consolidate all per-symbol data in a cache-friendly structure
- Provide computed properties (mid price, spread, PnL)
- Support multi-symbol strategies with O(1) state access

## Fields

| Field | Type | Description |
|-------|------|-------------|
| `book` | `NLevelOrderBook<512>` | Order book with 512 price levels |
| `position` | `Quantity` | Net position (positive=long, negative=short) |
| `avgEntryPrice` | `Price` | Volume-weighted average entry price |
| `lastTradePrice` | `Price` | Most recent trade price |
| `lastUpdateNs` | `int64_t` | Last update timestamp (nanoseconds) |
| `symbolId` | `SymbolId` | Symbol identifier |

## Computed Properties

### Mid Price

```cpp
std::optional<Price> mid() const noexcept;
```

Returns midpoint between best bid and ask. Returns `nullopt` if either side is missing.

### Book Spread

```cpp
std::optional<Price> bookSpread() const noexcept;
```

Returns bid-ask spread. Returns `nullopt` if book is one-sided.

### Unrealized PnL

```cpp
double unrealizedPnl(Price markPrice) const noexcept;
double unrealizedPnl() const noexcept;  // Uses mid() as mark
```

Calculates unrealized PnL based on current position and mark price.

### Position State

```cpp
bool isLong() const noexcept;   // position > 0
bool isShort() const noexcept;  // position < 0
bool isFlat() const noexcept;   // position == 0
```

### Reset

```cpp
void reset() noexcept;
```

Clears all state: book, position, prices, timestamps.

## Cross-Symbol Helpers

Free functions for multi-symbol analysis:

```cpp
// Price spread: midA - midB
std::optional<Price> spread(const SymbolContext& a, const SymbolContext& b);

// Price ratio: midA / midB
std::optional<double> ratio(const SymbolContext& a, const SymbolContext& b);
```

## Example

```cpp
class SpreadStrategy : public Strategy
{
public:
  SpreadStrategy(SymbolId front, SymbolId back, const SymbolRegistry& registry)
    : Strategy(1, {front, back}, registry), _front(front), _back(back) {}

protected:
  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override
  {
    // Get spread between front and back month
    auto spreadOpt = spread(ctx(_front), ctx(_back));
    if (!spreadOpt) return;

    double spreadValue = spreadOpt->toDouble();

    // Check unrealized PnL
    double pnl = ctx(_front).unrealizedPnl() + ctx(_back).unrealizedPnl();

    // Check position state
    if (ctx(_front).isLong() && pnl > _target)
    {
      closePosition();
    }
  }

private:
  SymbolId _front, _back;
  double _target{100.0};
};
```

## Memory Layout

`SymbolContext` is designed for cache efficiency:

- ~4KB per symbol (512-level book dominates)
- All fields in single contiguous struct
- Access via `SymbolStateMap` provides O(1) lookup

## See Also

- [Strategy](strategy.md) - Strategy base class
- [NLevelOrderBook](../book/nlevel_order_book.md) - Order book implementation
- [SymbolStateMap](symbol_state_map.md) - O(1) container for per-symbol state
