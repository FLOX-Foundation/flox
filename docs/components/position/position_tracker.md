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
double getRealizedPnl(SymbolId symbol) const;
double getTotalRealizedPnl() const;
CostBasisMethod method() const;
```

### Order Event Handlers

Inherited from `IOrderExecutionListener`:

```cpp
void onOrderFilled(const Order& order) override;
void onOrderPartiallyFilled(const Order& order, Quantity fillQty) override;
```

## Example Usage

### Backtest Integration

```cpp
BacktestConfig config;
config.initialCapital = 10000.0;
config.feeRate = 0.0004;

BacktestRunner runner(config);

// Create strategy
MyStrategy strategy(1, symbol);
runner.setStrategy(&strategy);

// Add position tracker
PositionTracker positions(2, CostBasisMethod::FIFO);
runner.addExecutionListener(&positions);

// Run backtest
auto result = runner.run(*reader);

// Check results
std::cout << "Position: " << positions.getPosition(symbol).toDouble() << "\n";
std::cout << "Avg entry: " << positions.getAvgEntryPrice(symbol).toDouble() << "\n";
std::cout << "Realized PnL: " << positions.getRealizedPnl(symbol) << "\n";
```

### Multiple Symbols

```cpp
PositionTracker tracker(1, CostBasisMethod::LIFO);

// Track fills from executor
executor.addExecutionListener(&tracker);

// Query per-symbol
for (SymbolId sym : symbols)
{
  std::cout << "Symbol " << sym << ": "
            << tracker.getPosition(sym).toDouble() << " @ "
            << tracker.getAvgEntryPrice(sym).toDouble() << "\n";
}

// Total across all symbols
std::cout << "Total realized PnL: " << tracker.getTotalRealizedPnl() << "\n";
```

## Internal Structure

### Lot-Based Tracking

```cpp
struct Lot
{
  double quantity;  // Signed: positive=long, negative=short
  double price;     // Entry price
};

struct PositionState
{
  std::deque<Lot> lots;    // Open lots
  double realizedPnl{0.0}; // Accumulated realized PnL

  double position() const;       // Sum of lot quantities
  double avgEntryPrice() const;  // VWAP of open lots
};
```

### Position Updates

1. **Opening trade**: Add new lot to deque
2. **Closing trade**: Remove lots per cost basis method, calculate realized PnL
3. **Flipping trade** (long to short): Close all, then open opposite

For AVERAGE method, lots are consolidated into single VWAP lot.

## Compliance Notes

- FIFO is required by IRS for tax reporting (US)
- LIFO may be preferred for tax optimization (where allowed)
- AVERAGE is common for mutual funds and some jurisdictions
- All methods track exact lot-level PnL for audit trails

## See Also

- [IPositionManager](abstract_position_manager.md) - Interface definition
- [Strategy](../strategy/strategy.md) - Strategy base class
- [BacktestRunner](../backtest/backtest_runner.md) - Backtesting framework
