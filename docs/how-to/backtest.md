# Running a Backtest

Complete example of backtesting an SMA crossover strategy.

## Build

Enable the backtest module:

```bash
cmake .. -DFLOX_ENABLE_BACKTEST=ON
make -j$(nproc)
```

## Strategy

```cpp
#include "flox/backtest/backtest_runner.h"
#include "flox/book/events/trade_event.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/strategy/signal_strategy.h"

#include <deque>

using namespace flox;

class SmaCrossover : public SignalStrategy
{
public:
  SmaCrossover(SymbolId symbol, size_t fast, size_t slow, Quantity size)
      : _symbol(symbol), _fast(fast), _slow(slow), _size(size) {}

  SubscriberId id() const override { return 1; }
  void start() override { _running = true; }
  void stop() override { _running = false; }

  void onTrade(const TradeEvent& ev) override
  {
    if (!_running || ev.trade.symbol != _symbol)
      return;

    _prices.push_back(ev.trade.price.toDouble());
    if (_prices.size() > _slow)
      _prices.pop_front();

    if (_prices.size() < _slow)
      return;

    double fast_sma = sma(_fast);
    double slow_sma = sma(_slow);
    bool above = fast_sma > slow_sma;

    if (above && !_prev_above && !_long)
    {
      if (_short) { emitMarketBuy(_symbol, _size); _short = false; }
      emitMarketBuy(_symbol, _size);
      _long = true;
    }
    else if (!above && _prev_above && !_short)
    {
      if (_long) { emitMarketSell(_symbol, _size); _long = false; }
      emitMarketSell(_symbol, _size);
      _short = true;
    }

    _prev_above = above;
  }

private:
  double sma(size_t n) const
  {
    double sum = 0;
    auto it = _prices.end();
    for (size_t i = 0; i < n; ++i)
      sum += *--it;
    return sum / n;
  }

  SymbolId _symbol;
  size_t _fast, _slow;
  Quantity _size;
  std::deque<double> _prices;
  bool _running{false}, _prev_above{false}, _long{false}, _short{false};
};
```

## Main

```cpp
int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " <data_dir> [symbol_id]\n";
    return 1;
  }

  std::filesystem::path data_dir = argv[1];
  uint32_t symbol_id = (argc > 2) ? std::stoul(argv[2]) : 1;

  // Load data
  replay::ReaderFilter filter;
  filter.symbols = {symbol_id};
  auto reader = replay::createMultiSegmentReader(data_dir, filter);

  // Configure backtest
  BacktestConfig config;
  config.initialCapital = 10000.0;
  config.feeRate = 0.0004;  // 0.04% taker fee

  BacktestRunner runner(config);

  // Create strategy
  SmaCrossover strategy(symbol_id, 10, 20, Quantity::fromDouble(1.0));
  runner.setSignalStrategy(&strategy);

  // Run
  BacktestResult result = runner.run(*reader);
  auto stats = result.computeStats();

  // Output
  std::cout << "Initial: " << stats.initialCapital << "\n";
  std::cout << "Final:   " << stats.finalCapital << "\n";
  std::cout << "Return:  " << stats.returnPct << "%\n";
  std::cout << "Trades:  " << stats.totalTrades << "\n";
  std::cout << "Win rate: " << stats.winRate * 100 << "%\n";
  std::cout << "Sharpe:  " << stats.sharpeRatio << "\n";
  std::cout << "Max DD:  " << stats.maxDrawdownPct << "%\n";

  return 0;
}
```

## Output Example

```
Initial: 10000
Final:   10245.3
Return:  2.453%
Trades:  47
Win rate: 51.0638%
Sharpe:  1.23
Max DD:  3.21%
```
