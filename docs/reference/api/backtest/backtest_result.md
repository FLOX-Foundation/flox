# BacktestResult

`BacktestResult` collects fills during backtest execution and computes performance statistics.

## BacktestConfig

```cpp
struct BacktestConfig
{
  double initialCapital{100000.0};
  double feeRate{0.0001};        // 0.01% per trade
  bool usePercentageFee{true};
  double fixedFeePerTrade{0.0};
};
```

| Field | Default | Description |
|-------|---------|-------------|
| `initialCapital` | 100000.0 | Starting capital |
| `feeRate` | 0.0001 | Fee as fraction of notional (0.01%) |
| `usePercentageFee` | true | Use percentage fee vs fixed |
| `fixedFeePerTrade` | 0.0 | Fixed fee per trade |

## BacktestStats

```cpp
struct BacktestStats
{
  size_t totalTrades{0};
  size_t winningTrades{0};
  size_t losingTrades{0};

  double initialCapital{0.0};
  double finalCapital{0.0};
  double totalPnl{0.0};
  double totalFees{0.0};
  double netPnl{0.0};
  double grossProfit{0.0};
  double grossLoss{0.0};

  double maxDrawdown{0.0};
  double maxDrawdownPct{0.0};

  double winRate{0.0};
  double profitFactor{0.0};
  double avgWin{0.0};
  double avgLoss{0.0};

  double sharpeRatio{0.0};
  double returnPct{0.0};

  UnixNanos startTimeNs{0};
  UnixNanos endTimeNs{0};
};
```

## BacktestResult

```cpp
class BacktestResult
{
public:
  explicit BacktestResult(const BacktestConfig& config = {}, size_t expectedFills = 16384);

  void recordFill(const Fill& fill);
  BacktestStats computeStats() const noexcept;

  const std::vector<Fill>& fills() const noexcept;
  const std::vector<TradeRecord>& trades() const noexcept;
  double totalPnl() const noexcept;
};
```

## Metrics

| Metric | Formula |
|--------|---------|
| `netPnl` | `totalPnl - totalFees` |
| `returnPct` | `(netPnl / initialCapital) * 100` |
| `winRate` | `winningTrades / totalTrades` |
| `profitFactor` | `grossProfit / grossLoss` |
| `avgWin` | `grossProfit / winningTrades` |
| `avgLoss` | `grossLoss / losingTrades` |
| `maxDrawdownPct` | `maxDrawdown / peakEquity * 100` |
| `sharpeRatio` | `mean(returns) / std(returns) * sqrt(252)` |

## Fee Calculation

```cpp
// Percentage fee (default)
fee = price * quantity * feeRate

// Fixed fee
fee = fixedFeePerTrade
```

## Usage

```cpp
auto stats = result.computeStats();

std::cout << "Return: " << stats.returnPct << "%\n";
std::cout << "Sharpe: " << stats.sharpeRatio << "\n";
std::cout << "Max DD: " << stats.maxDrawdownPct << "%\n";
std::cout << "Win rate: " << stats.winRate * 100 << "%\n";
std::cout << "Profit factor: " << stats.profitFactor << "\n";
```
