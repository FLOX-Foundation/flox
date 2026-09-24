# PositionTracker

`PositionTracker` implements `IPositionManager` with full support for FIFO, LIFO, and Average cost basis methods. This is critical for compliance and accurate PnL reporting.

```cpp
enum class CostBasisMethod { FIFO, LIFO, AVERAGE };

class PositionTracker : public IPositionManager
{
public:
  PositionTracker(SubscriberId id, CostBasisMethod method = CostBasisMethod::FIFO);
};
```

## Purpose

- Track positions across all symbols
- Calculate realized PnL using configurable cost basis method
- Provide average entry price for position sizing and risk management
- Thread-safe access from multiple components

## Cost Basis Methods

### FIFO (First In, First Out)

Oldest lots are closed first. Default method, required by many tax jurisdictions.

```
Buy 10 @ $100
Buy 10 @ $110
Sell 15 @ $120

Realized PnL:
- Close 10 @ $100: ($120 - $100) * 10 = $200
- Close 5 @ $110: ($120 - $110) * 5 = $50
Total: $250

Remaining: 5 @ $110
```

### LIFO (Last In, First Out)

Newest lots are closed first.

```
Buy 10 @ $100
Buy 10 @ $110
Sell 15 @ $120

Realized PnL:
- Close 10 @ $110: ($120 - $110) * 10 = $100
- Close 5 @ $100: ($120 - $100) * 5 = $100
Total: $200

Remaining: 5 @ $100
```

### AVERAGE (Volume-Weighted Average)

All lots consolidated into single VWAP position.

```
Buy 10 @ $100
Buy 10 @ $110
Avg price: (10*100 + 10*110) / 20 = $105

Sell 15 @ $120
Realized PnL: ($120 - $105) * 15 = $225

Remaining: 5 @ $105
```

## API

### Constructor

```cpp
PositionTracker(SubscriberId id, CostBasisMethod method = CostBasisMethod::FIFO);
```

### Position Queries

```cpp
Quantity getPosition(SymbolId symbol) const override;
Price getAvgEntryPrice(SymbolId symbol) const;
std::optional<Price> getAverageEntryPrice(SymbolId symbol) const override;
Volume getRealizedPnl(SymbolId symbol) const;
Volume getTotalRealizedPnl() const;
size_t trackedSymbolCount() const;
CostBasisMethod method() const;
```

`getAvgEntryPrice` returns a default-constructed `Price` on a flat position.
`getAverageEntryPrice` is the `IPositionManager` override and returns nothing
there instead, so a caller cannot mistake "flat" for "entered at zero". The
strategy context reads the override; see
[IPositionManager](abstract_position_manager.md).

All position queries are read-only: querying a symbol that never traded does
not create an entry for it. `_states` is a plain (non-`mutable`)
`SymbolStateMap`, so these `const` methods bind `SymbolStateMap`'s `const
operator[]`, which never marks a symbol initialized and never allocates an
overflow entry. `trackedSymbolCount()` (`_states.size()` under the lock)
exists to make that observable: it only grows from an actual fill, never
from a query.

### Order Event Handlers

Inherited from `IOrderExecutionListener`:

```cpp
void onOrderFilled(const Order& order) override;
void onOrderPartiallyFilled(const Order& order, Quantity fillQty) override;

// Preferred: these carry the price the fill happened at, which a market
// order does not carry on the order itself.
void onOrderFilled(const Order& order, Quantity fillQty, Price fillPrice) override;
void onOrderPartiallyFilled(const Order& order, Quantity fillQty,
                            Price fillPrice) override;
```

## Example Usage

### Backtest Integration

```cpp
BacktestConfig config;
config.initialCapital = 10000.0;
config.feeRate = 0.0004;

BacktestRunner runner(config);

// Create strategy. Both Strategy constructors require a const SymbolRegistry&.
MyStrategy strategy(1, symbol, registry);
runner.setStrategy(&strategy);

// Add position tracker
PositionTracker positions(2, CostBasisMethod::FIFO);
runner.addExecutionListener(&positions);

// Run backtest
auto result = runner.run(*reader);

// Check results
std::cout << "Position: " << positions.getPosition(symbol).toDouble() << "\n";
std::cout << "Avg entry: " << positions.getAvgEntryPrice(symbol).toDouble() << "\n";
std::cout << "Realized PnL: " << positions.getRealizedPnl(symbol).toDouble() << "\n";  // Volume
```

### Multiple Symbols

```cpp
PositionTracker tracker(1, CostBasisMethod::LIFO);

// Track fills. BacktestRunner::addExecutionListener is the registration point;
// SimulatedExecutor has no addExecutionListener, only a single
// setOrderEventCallback(std::function<void(const OrderEvent&)>).
runner.addExecutionListener(&tracker);

// Query per-symbol
for (SymbolId sym : symbols)
{
  std::cout << "Symbol " << sym << ": "
            << tracker.getPosition(sym).toDouble() << " @ "
            << tracker.getAvgEntryPrice(sym).toDouble() << "\n";
}

// Total across all symbols
std::cout << "Total realized PnL: " << tracker.getTotalRealizedPnl().toDouble() << "\n";
```

## Internal Structure

### Lot-Based Tracking

```cpp
struct Lot
{
  Quantity quantity;  // Signed: positive=long, negative=short
  Price price;        // Entry price (fixed-point)
};

struct PositionState
{
  std::deque<Lot> lots;   // Open lots
  Volume realizedPnl{};   // Accumulated realized PnL (fixed-point money)

  Quantity position() const;    // Sum of lot quantities
  Price avgEntryPrice() const;  // VWAP of open lots
};
```

### Position Updates

1. **Opening trade**: Add new lot to deque
2. **Closing trade**: Remove lots per cost basis method, calculate realized PnL
3. **Flipping trade** (long to short): Close all, then open opposite

For AVERAGE method, lots are consolidated into single VWAP lot.

## Thread Safety

All public methods are protected by `std::mutex`:
- Safe for concurrent access from multiple threads
- Position queries can be called while fills are being processed
- Uses `SymbolStateMap` for O(1) per-symbol access

## Fixed-Point Arithmetic

Every step of the cost basis and of realised PnL is computed in the fixed-point
types, with no `double` anywhere in between:

- A realisation is `mulDivI64(closePrice - lotPrice, closeQty, Volume::Scale)`
  per lot, accumulated with `checkedAddI64`. The subtraction is checked too.
- The weighted average entry price carries each `quantity * price` as a
  quotient and a remainder against the scale, so the sum of the products is
  exact however many lots there are; the single division at the end is the only
  rounding step, and it is unavoidable (an average price need not be
  representable at 1e-8). The AVERAGE method re-derives the running average
  from the two weighted prices rather than from the previous average's rounded
  form, so the rounding does not compound over a session.
- Sums saturate at the int64 boundary rather than wrapping, and trip a scale
  check in a checked build.

The results are therefore exact to the raw and identical on every toolchain.
The older implementation computed the price difference, the product and the
weighted average as `double` and converted back, which drifted once a price
outgrew the double mantissa -- and drifted by a different amount depending on
FMA contraction, so two builds of the same code disagreed on the same tape.

## Compliance Notes

- FIFO is required by IRS for tax reporting (US)
- LIFO may be preferred for tax optimization (where allowed)
- AVERAGE is common for mutual funds and some jurisdictions
- All methods track exact lot-level PnL for audit trails

## Migration Notes

`getRealizedPnl()` and `getTotalRealizedPnl()` return `Volume`, the engine's
money type. They returned `double` first and then `Price`; `Price` was wrong
because realised PnL is a quantity times a price difference -- a notional, not
a price -- and the wrong type let it be compared with, assigned to and added to
an actual price with no diagnostic.

```cpp
// Old API
Price pnl = tracker.getRealizedPnl(symbol);

// New API
Volume pnl = tracker.getRealizedPnl(symbol);
double pnlDouble = pnl.toDouble();  // If double needed
```

`.toDouble()` call sites (the C API, the Python and Node bindings) are
unaffected; code that stored the result in a `Price` has to change the type.

## See Also

- [IPositionManager](abstract_position_manager.md) - Interface definition
- [Strategy](../strategy/strategy.md) - Strategy base class
- [BacktestRunner](../backtest/backtest_runner.md) - Backtesting framework
