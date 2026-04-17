# BacktestResult

`BacktestResult` collects fills during backtest execution and computes performance statistics, including an equity curve and a range of risk/reward metrics.

## BacktestConfig

```cpp
struct BacktestConfig
{
  double initialCapital{100000.0};
  double feeRate{0.0001};            // 0.01% per trade
  bool usePercentageFee{true};
  double fixedFeePerTrade{0.0};

  SlippageProfile defaultSlippage{};
  std::vector<std::pair<SymbolId, SlippageProfile>> perSymbolSlippage{};

  QueueModel queueModel{QueueModel::NONE};
  size_t queueDepth{8};

  double riskFreeRate{0.0};
  double metricsAnnualizationFactor{252.0};
};
```

| Field | Default | Description |
|-------|---------|-------------|
| `initialCapital` | 100000.0 | Starting capital |
| `feeRate` | 0.0001 | Fee as fraction of notional (0.01%) |
| `usePercentageFee` | true | Use percentage fee vs fixed |
| `fixedFeePerTrade` | 0.0 | Fixed fee per trade when percentage is off |
| `defaultSlippage` | `{NONE}` | Slippage profile applied when no per-symbol override exists. See `slippage.md`. |
| `perSymbolSlippage` | `{}` | Per-symbol overrides of the default profile |
| `queueModel` | `NONE` | Queue simulation mode for limit orders. See `queue_simulation.md`. |
| `queueDepth` | 8 | Levels tracked in `FULL` mode |
| `riskFreeRate` | 0.0 | Per-period risk-free rate subtracted from each trade return before Sharpe/Sortino |
| `metricsAnnualizationFactor` | 252.0 | Scaling factor used for annualized Sharpe/Sortino/Calmar (sqrt applied) |

## BacktestStats

```cpp
struct BacktestStats
{
  size_t totalTrades;
  size_t winningTrades;
  size_t losingTrades;

  double initialCapital;
  double finalCapital;
  double totalPnl;
  double totalFees;
  double netPnl;
  double grossProfit;
  double grossLoss;

  double maxDrawdown;
  double maxDrawdownPct;

  double winRate;
  double profitFactor;
  double avgWin;
  double avgLoss;
  double avgWinLossRatio;

  size_t maxConsecutiveWins;
  size_t maxConsecutiveLosses;

  double avgTradeDurationNs;
  double medianTradeDurationNs;
  double maxTradeDurationNs;

  double sharpeRatio;
  double sortinoRatio;
  double calmarRatio;
  double timeWeightedReturn;
  double returnPct;

  UnixNanos startTimeNs;
  UnixNanos endTimeNs;
};
```

### New metrics

| Metric | Description |
|--------|-------------|
| `avgWinLossRatio` | `avgWin / avgLoss` (undefined when no losses) |
| `maxConsecutiveWins` | Longest streak of profitable trades |
| `maxConsecutiveLosses` | Longest streak of losing trades |
| `avgTradeDurationNs` | Mean time from entry to exit in nanoseconds |
| `medianTradeDurationNs` | Median trade duration in nanoseconds |
| `maxTradeDurationNs` | Longest trade duration in nanoseconds |
| `timeWeightedReturn` | Cumulative product of per-trade returns minus 1 |

### Formula changes

Sharpe, Sortino and Calmar are computed from per-trade returns against `initialCapital`, annualized with `sqrt(metricsAnnualizationFactor)`. `riskFreeRate` is subtracted from each return before the stats are computed. Calmar is `annualizedReturn / maxDrawdownPct`.

## EquityPoint

```cpp
struct EquityPoint
{
  UnixNanos timestampNs;
  double equity;
  double drawdownPct;
};
```

One point is appended to the equity curve on every closed trade. The curve is accessible via `BacktestResult::equityCurve()` and can be written to a CSV file with header `timestamp_ns,equity,drawdown_pct` via `writeEquityCurveCsv(path)`.

## BacktestResult

```cpp
class BacktestResult
{
public:
  explicit BacktestResult(const BacktestConfig& config = {}, size_t expectedFills = 0);

  void recordFill(const Fill& fill);
  BacktestStats computeStats() const;

  const std::vector<Fill>& fills() const;
  const std::vector<TradeRecord>& trades() const;
  const std::vector<EquityPoint>& equityCurve() const;
  double totalPnl() const;

  bool writeEquityCurveCsv(const std::string& path) const;
};
```

Each `TradeRecord` now carries both the entry and exit price, the closed quantity, and the entry/exit timestamps. These fields drive the duration statistics and enable equity-curve analysis per trade.

## Usage

```cpp
BacktestConfig cfg;
cfg.defaultSlippage = {SlippageModel::FIXED_BPS, 0, 2.0, 0.0};  // 2 bps
cfg.queueModel = QueueModel::TOB;
cfg.riskFreeRate = 0.0;

BacktestResult result(cfg);
// feed fills via result.recordFill(...) or by running a BacktestRunner
auto stats = result.computeStats();

std::cout << "Return: " << stats.returnPct << "%\n";
std::cout << "Sharpe: " << stats.sharpeRatio << "\n";
std::cout << "Sortino: " << stats.sortinoRatio << "\n";
std::cout << "Calmar: " << stats.calmarRatio << "\n";
std::cout << "Max consecutive wins: " << stats.maxConsecutiveWins << "\n";
std::cout << "Avg duration (s): " << stats.avgTradeDurationNs / 1e9 << "\n";

result.writeEquityCurveCsv("equity.csv");
```
