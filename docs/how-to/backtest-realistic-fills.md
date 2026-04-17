# Realistic backtest fills

This guide walks through configuring a backtest with realistic fills: slippage on market orders, queue simulation for limit orders, and an exported equity curve for analysis.

## 1. Configure the backtest

```cpp
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"

flox::BacktestConfig cfg;
cfg.initialCapital = 100'000.0;
cfg.feeRate = 0.0002;  // 2 bps per fill
cfg.defaultSlippage = {flox::SlippageModel::FIXED_BPS, 0, 1.0, 0.0};  // 1 bps default
cfg.queueModel = flox::QueueModel::TOB;
cfg.riskFreeRate = 0.0;
cfg.metricsAnnualizationFactor = 252.0;

flox::BacktestRunner runner(cfg);
```

Per-symbol overrides extend the default:

```cpp
cfg.perSymbolSlippage.emplace_back(
    kEthUsd, flox::SlippageProfile{flox::SlippageModel::VOLUME_IMPACT, 0, 0.0, 0.01});
```

## 2. Run it

```cpp
runner.setStrategy(&yourStrategy);
auto result = runner.run(*reader);  // reader is a replay::IMultiSegmentReader
```

## 3. Inspect the stats

```cpp
auto stats = result.computeStats();
fmt::print("Trades: {}\n", stats.totalTrades);
fmt::print("Net PnL: {:.2f}\n", stats.netPnl);
fmt::print("Sharpe: {:.3f}\n", stats.sharpeRatio);
fmt::print("Sortino: {:.3f}\n", stats.sortinoRatio);
fmt::print("Calmar: {:.3f}\n", stats.calmarRatio);
fmt::print("Max consecutive wins: {}\n", stats.maxConsecutiveWins);
fmt::print("Avg trade duration: {:.2f}s\n", stats.avgTradeDurationNs / 1e9);
```

## 4. Export the equity curve

```cpp
for (const auto& pt : result.equityCurve())
{
    fmt::print("{},{:.2f},{:.2f}\n", pt.timestampNs, pt.equity, pt.drawdownPct);
}

result.writeEquityCurveCsv("equity.csv");
```

The CSV has the header `timestamp_ns,equity,drawdown_pct` and one row per closed trade.

## 5. Python

```python
import flox

exec_ = flox.SimulatedExecutor()
exec_.set_default_slippage("fixed_bps", bps=1.0)
exec_.set_queue_model("tob")

# ... feed book updates, trades, and orders ...

result = flox.BacktestResult(initial_capital=100_000, fee_rate=0.0002)
result.ingest_executor(exec_)
print(result.stats())
equity = result.equity_curve()           # numpy structured array
result.write_equity_curve_csv("equity.csv")
```

## 6. JavaScript (QuickJS)

```js
const exec = new SimulatedExecutor();
exec.setDefaultSlippage("fixed_bps", 0, 1.0, 0);
exec.setQueueModel("tob", 1);

// ... feed data and orders ...

const result = new BacktestResult(100000, 0.0002, true, 0, 0, 252);
result.ingestExecutor(exec);
const stats = result.stats();
const curve = result.equityCurve();
result.writeEquityCurveCsv("equity.csv");
```

## 7. Codon

```python
from flox.backtest import SimulatedExecutor, BacktestResult
from flox.backtest import SLIPPAGE_FIXED_BPS, QUEUE_TOB

exec_ = SimulatedExecutor()
exec_.set_default_slippage(SLIPPAGE_FIXED_BPS, 0, 1.0, 0.0)
exec_.set_queue_model(QUEUE_TOB, 1)

# feed market data + orders ...

result = BacktestResult(initial_capital=100000.0, fee_rate=0.0002)
result.ingest_executor(exec_)
result.write_equity_curve_csv("equity.csv")
```
