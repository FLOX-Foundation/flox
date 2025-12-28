# Backtesting

Run your strategy against recorded market data.

## Prerequisites

- Completed [Recording Data](recording-data.md)
- Recorded `.floxlog` files
- Build with `-DFLOX_ENABLE_BACKTEST=ON`

## 1. BacktestRunner Overview

`BacktestRunner` replays recorded data through your strategy with simulated execution:

```
.floxlog files → BacktestRunner → Strategy → SimulatedExecutor → BacktestResult
```

## 2. Minimal Example

```cpp
#include "flox/backtest/backtest_runner.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/engine/symbol_registry.h"
#include "flox/strategy/strategy.h"

using namespace flox;

int main()
{
  // 1. Load recorded data
  replay::ReaderFilter filter;
  filter.symbols = {1};  // Only symbol ID 1
  auto reader = replay::createMultiSegmentReader("/data/market_data", filter);

  // 2. Configure backtest
  BacktestConfig config;
  config.initialCapital = 10000.0;
  config.feeRate = 0.0004;  // 0.04% taker fee

  BacktestRunner runner(config);

  // 3. Create registry and strategy
  SymbolRegistry registry;
  SymbolInfo info;
  info.exchange = "BINANCE";
  info.symbol = "BTCUSDT";
  info.tickSize = Price::fromDouble(0.01);
  SymbolId symbolId = registry.registerSymbol(info);

  MyStrategy strategy(symbolId, registry);
  runner.setStrategy(&strategy);

  // 4. Run
  BacktestResult result = runner.run(*reader);

  // 5. Get statistics
  auto stats = result.computeStats();
  std::cout << "Return: " << stats.returnPct << "%\n";
  std::cout << "Sharpe: " << stats.sharpeRatio << "\n";
  std::cout << "Max DD: " << stats.maxDrawdownPct << "%\n";
  std::cout << "Trades: " << stats.totalTrades << "\n";
}
```

## 3. Writing a Backtest-Ready Strategy

Use `Strategy` base class with signal emission:

```cpp
class MyStrategy : public Strategy
{
public:
  MyStrategy(SymbolId sym, const SymbolRegistry& registry)
    : Strategy(1, sym, registry) {}

  void start() override { _running = true; }
  void stop() override { _running = false; }

protected:
  void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) override
  {
    if (!_running) return;

    // Your logic here
    if (shouldBuy(ev.trade.price))
    {
      emitMarketBuy(symbol(), Quantity::fromDouble(1.0));
    }
  }

private:
  bool _running{false};
};
```

Key points:

- Inherit from `Strategy`, not `IStrategy`
- Use `emitMarketBuy()` / `emitMarketSell()` — BacktestRunner intercepts these signals
- Access order book via `ctx.book`, position via `ctx.position`

## 4. BacktestConfig Options

```cpp
BacktestConfig config;
config.initialCapital = 10000.0;  // Starting capital
config.feeRate = 0.0004;          // 0.04% per trade
config.slippage = 0.0;            // Price slippage (0 = fill at signal price)
```

## 5. BacktestResult Statistics

```cpp
auto stats = result.computeStats();

stats.initialCapital;    // Starting capital
stats.finalCapital;      // Ending capital
stats.returnPct;         // Total return %
stats.totalTrades;       // Number of trades
stats.winRate;           // Win rate (0-1)
stats.sharpeRatio;       // Annualized Sharpe
stats.sortinoRatio;      // Sortino ratio
stats.maxDrawdownPct;    // Maximum drawdown %
stats.profitFactor;      // Gross profit / gross loss
```

## 6. Time Range Filtering

Backtest only a portion of your data:

```cpp
replay::ReaderFilter filter;
filter.from_ns = 1704067200000000000LL;  // 2024-01-01
filter.to_ns = 1704153600000000000LL;    // 2024-01-02
filter.symbols = {1};

auto reader = replay::createMultiSegmentReader("/data", filter);
```

## 7. Using Bars Instead of Raw Data

For bar-based strategies, aggregate on the fly:

```cpp
class BarStrategy : public Strategy
{
public:
  BarStrategy(SymbolId sym, const SymbolRegistry& registry)
    : Strategy(1, sym, registry)
  {
    _aggregator = std::make_unique<TimeBarAggregator>(
      TimeBarPolicy(std::chrono::seconds(60)),
      this  // Strategy receives BarEvents
    );
  }

protected:
  void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) override
  {
    _aggregator->onTrade(ev);
  }

  void onBar(const BarEvent& ev) override
  {
    // Your bar-based logic
    if (ev.bar.close > ev.bar.open)
    {
      emitMarketBuy(symbol(), Quantity::fromDouble(1.0));
    }
  }

private:
  std::unique_ptr<TimeBarAggregator> _aggregator;
};
```

## 8. Inspecting Data Before Backtest

```cpp
auto summary = replay::BinaryLogReader::inspect("/data/market_data");

std::cout << "Events: " << summary.total_events << "\n";
std::cout << "Duration: " << summary.durationHours() << " hours\n";
std::cout << "Segments: " << summary.segment_count << "\n";
```

## 9. Performance Tips

1. **Build in Release mode**: `cmake .. -DCMAKE_BUILD_TYPE=Release`
2. **Filter symbols**: Only load what you need
3. **Disable logging**: Comment out `FLOX_LOG` calls
4. **Use indexed data**: Enables fast seeking

## Next Steps

- [Running a Backtest](../how-to/backtest.md) — Complete SMA crossover example
- [Grid Search Optimization](../how-to/grid-search.md) — Find optimal parameters
- [Bar Aggregation](../how-to/bar-aggregation.md) — Pre-aggregate for faster backtests
