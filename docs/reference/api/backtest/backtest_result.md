# BacktestResult

Container for backtest execution results and statistics computation.

## Overview

`BacktestResult` tracks fills, computes trades, and calculates performance metrics. Used by `BacktestRunner` and `BacktestOptimizer`.

## Header

```cpp
#include "flox/backtest/backtest_result.h"
```

## BacktestConfig

```cpp
struct BacktestConfig {
  double initialCapital{100000.0};
  double feeRate{0.0001};        // 0.01% per trade
  bool usePercentageFee{true};
  double fixedFeePerTrade{0.0};
};
```

## TradeRecord

```cpp
struct TradeRecord {
  SymbolId symbol{};
  Side side{};
  int64_t entryPriceRaw{0};
  int64_t exitPriceRaw{0};
  int64_t quantityRaw{0};
  UnixNanos entryTimeNs{0};
  UnixNanos exitTimeNs{0};
  int64_t pnlRaw{0};
  int64_t feeRaw{0};
};
```

## BacktestStats

```cpp
struct BacktestStats {
  // Trade counts
  size_t totalTrades{0};
  size_t winningTrades{0};
  size_t losingTrades{0};

  // Capital
  double initialCapital{0.0};
  double finalCapital{0.0};
  double totalPnl{0.0};
  double totalFees{0.0};
  double netPnl{0.0};
  double grossProfit{0.0};
  double grossLoss{0.0};

  // Risk
  double maxDrawdown{0.0};
  double maxDrawdownPct{0.0};

  // Performance ratios
  double winRate{0.0};
  double profitFactor{0.0};
  double avgWin{0.0};
  double avgLoss{0.0};

  // Risk-adjusted metrics
  double sharpeRatio{0.0};
  double sortinoRatio{0.0};
  double calmarRatio{0.0};
  double returnPct{0.0};

  // Time range
  UnixNanos startTimeNs{0};
  UnixNanos endTimeNs{0};
};
```

## Class Definition

```cpp
class BacktestResult {
public:
  static constexpr size_t kMaxSymbols = 256;

  explicit BacktestResult(const BacktestConfig& config = {}, size_t expectedFills = 0);

  void recordFill(const Fill& fill);
  BacktestStats computeStats() const;

  const BacktestConfig& config() const;
  const std::vector<Fill>& fills() const;
  const std::vector<TradeRecord>& trades() const;
  double totalPnl() const;
};
```

## Methods

### Constructor

```cpp
explicit BacktestResult(const BacktestConfig& config = {}, size_t expectedFills = 0);
```

Create result container. Optionally reserve space for expected fills.

### recordFill

```cpp
void recordFill(const Fill& fill);
```

Record an order fill. Automatically tracks positions and computes trades when positions are closed.

### computeStats

```cpp
BacktestStats computeStats() const;
```

Compute all performance statistics from recorded fills and trades.

### Accessors

```cpp
const BacktestConfig& config() const;      // Get config
const std::vector<Fill>& fills() const;    // All fills
const std::vector<TradeRecord>& trades() const;  // Completed trades
double totalPnl() const;                   // Running PnL
```

## Example

```cpp
BacktestConfig config;
config.initialCapital = 50000.0;
config.feeRate = 0.0002;  // 0.02%

BacktestResult result(config);

// Record fills from strategy execution
Fill buyFill;
buyFill.orderId = 1;
buyFill.symbol = 1;
buyFill.side = Side::BUY;
buyFill.price = Price::fromDouble(100.0);
buyFill.quantity = Quantity::fromDouble(10.0);
buyFill.timestampNs = 1000000;
result.recordFill(buyFill);

Fill sellFill;
sellFill.orderId = 2;
sellFill.symbol = 1;
sellFill.side = Side::SELL;
sellFill.price = Price::fromDouble(105.0);
sellFill.quantity = Quantity::fromDouble(10.0);
sellFill.timestampNs = 2000000;
result.recordFill(sellFill);

// Compute statistics
auto stats = result.computeStats();
std::cout << "Net PnL: " << stats.netPnl << "\n";
std::cout << "Sharpe: " << stats.sharpeRatio << "\n";
std::cout << "Max DD: " << stats.maxDrawdownPct << "%\n";
```

## Notes

- Positions tracked per symbol using flat array (fast) with overflow for > 256 symbols
- PnL computed in raw int64 format to avoid floating-point errors
- Sharpe/Sortino/Calmar ratios use trade returns
- Win rate = winning trades / total trades

## See Also

- [BacktestRunner](./backtest_runner.md)
- [BacktestOptimizer](./backtest_optimizer.md)
- [How-to: Backtesting](../../../how-to/backtest.md)
