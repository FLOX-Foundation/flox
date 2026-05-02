# Running a Backtest

Replay an SMA crossover strategy against historical data and read the stats out.

## Build / install

=== "Python"

    ```bash
    pip install flox-py
    ```

    Or build from source: `cmake -B build -DFLOX_ENABLE_PYTHON=ON -DFLOX_ENABLE_BACKTEST=ON && cmake --build build` and put `build/python` on `PYTHONPATH`.

=== "Node.js"

    ```bash
    npm install @flox-foundation/flox
    ```

=== "Codon"

    Build flox with `-DFLOX_ENABLE_CAPI=ON -DFLOX_ENABLE_BACKTEST=ON`, then point Codon at `codon/flox`.

=== "C++"

    ```bash
    cmake -B build -DFLOX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ```

## Strategy

A 10/20 SMA crossover. Buy when fast crosses above slow, sell on the reverse.

=== "Python"

    ```python
    import flox_py as flox
    from collections import deque

    class SmaCrossover(flox.Strategy):
        def __init__(self, symbols, fast=10, slow=20, size=1.0):
            super().__init__(symbols)
            self.fast, self.slow, self.size = fast, slow, size
            self.prices = deque(maxlen=slow)
            self.prev_above = False

        def on_trade(self, ctx, trade):
            self.prices.append(trade.price)
            if len(self.prices) < self.slow:
                return
            fast_sma = sum(list(self.prices)[-self.fast:]) / self.fast
            slow_sma = sum(self.prices) / self.slow
            above = fast_sma > slow_sma
            if above and not self.prev_above and ctx.is_flat():
                self.market_buy(self.size)
            elif not above and self.prev_above and ctx.is_long():
                self.market_sell(self.size)
            self.prev_above = above
    ```

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');

    class SmaCrossover {
      constructor(symbols, fast = 10, slow = 20, size = 1.0) {
        this.symbols = symbols;
        this.fast = fast; this.slow = slow; this.size = size;
        this.prices = []; this.prevAbove = false;
      }
      onTrade(ctx, trade, emit) {
        this.prices.push(trade.price);
        if (this.prices.length > this.slow) this.prices.shift();
        if (this.prices.length < this.slow) return;
        const fastSma = this.prices.slice(-this.fast).reduce((a,b)=>a+b)/this.fast;
        const slowSma = this.prices.reduce((a,b)=>a+b)/this.slow;
        const above = fastSma > slowSma;
        if (above && !this.prevAbove && ctx.position === 0) emit.marketBuy(this.size);
        else if (!above && this.prevAbove && ctx.position > 0) emit.marketSell(this.size);
        this.prevAbove = above;
      }
    }
    ```

=== "Codon"

    ```python
    from flox.strategy import Strategy
    from flox.context import SymbolContext
    from flox.types import TradeData
    from flox.indicators import StreamingSMA

    class SmaCrossover(Strategy):
        fast: StreamingSMA
        slow: StreamingSMA
        size: float
        prev_above: bool

        def __init__(self, symbols: List[int], fast_n: int = 10, slow_n: int = 20, size: float = 1.0):
            super().__init__(symbols)
            self.fast = StreamingSMA(fast_n)
            self.slow = StreamingSMA(slow_n)
            self.size = size
            self.prev_above = False

        def on_trade(self, ctx: SymbolContext, trade: TradeData):
            f = self.fast.update(trade.price.to_double())
            s = self.slow.update(trade.price.to_double())
            if f is None or s is None:
                return
            above = f > s
            if above and not self.prev_above and self.position() == 0.0:
                self.market_buy(self.size)
            elif not above and self.prev_above and self.position() > 0.0:
                self.market_sell(self.size)
            self.prev_above = above
    ```

=== "C++"

    ```cpp
    #include "flox/strategy/strategy.h"
    #include <deque>

    using namespace flox;

    class SmaCrossover : public Strategy {
    public:
      SmaCrossover(SymbolId symbol, size_t fast, size_t slow, Quantity size,
                   const SymbolRegistry& registry)
          : Strategy(1, symbol, registry), _fast(fast), _slow(slow), _size(size) {}

      void start() override { _running = true; }
      void stop() override  { _running = false; }

    protected:
      void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& ev) override
      {
        if (!_running) return;
        _prices.push_back(ev.trade.price.toDouble());
        if (_prices.size() > _slow) _prices.pop_front();
        if (_prices.size() < _slow) return;

        double fast_sma = sma(_fast), slow_sma = sma(_slow);
        bool above = fast_sma > slow_sma;
        if (above && !_prev_above && !_long) { emitMarketBuy(symbol(), _size); _long = true; _short = false; }
        else if (!above && _prev_above && !_short) { emitMarketSell(symbol(), _size); _short = true; _long = false; }
        _prev_above = above;
      }

    private:
      double sma(size_t n) const {
        double sum = 0; auto it = _prices.end();
        for (size_t i = 0; i < n; ++i) sum += *--it;
        return sum / n;
      }
      size_t _fast, _slow;
      Quantity _size;
      std::deque<double> _prices;
      bool _running{false}, _prev_above{false}, _long{false}, _short{false};
    };
    ```

## Run the backtest

=== "Python"

    ```python
    reg = flox.SymbolRegistry()
    btc = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)

    strat = SmaCrossover([btc])
    bt = flox.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000)
    bt.set_strategy(strat)

    stats = bt.run_csv("data/btcusdt_1m.csv", "BTCUSDT")
    print(f"Final ${stats['final_capital']:.2f}  return {stats['return_pct']:.2f}%  "
          f"trades {stats['total_trades']}  Sharpe {stats['sharpe']:.2f}  "
          f"DD {stats['max_drawdown_pct']:.2f}%")
    ```

=== "Node.js"

    ```javascript
    const reg = new flox.SymbolRegistry();
    const btc = reg.addSymbol("binance", "BTCUSDT", 0.01);

    const strat = new SmaCrossover([btc]);
    const bt = new flox.BacktestRunner(reg, 0.0004, 10000);
    bt.setStrategy(strat);

    const stats = bt.runCsv("data/btcusdt_1m.csv", "BTCUSDT");
    console.log(`Final $${stats.finalCapital.toFixed(2)}  return ${stats.returnPct.toFixed(2)}%  ` +
                `trades ${stats.totalTrades}  Sharpe ${stats.sharpeRatio.toFixed(2)}  ` +
                `DD ${stats.maxDrawdownPct.toFixed(2)}%`);
    ```

=== "Codon"

    ```python
    from flox.runner import BacktestRunner

    reg = ...                                # see flox.engine in your runner code
    btc = reg.add_symbol("binance", "BTCUSDT", 0.01)

    strat = SmaCrossover([btc])
    bt = BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(strat)
    stats = bt.run_csv("data/btcusdt_1m.csv", "BTCUSDT")
    print(f"Final ${stats.final_capital:.2f}  return {stats.return_pct:.2f}%")
    ```

=== "C++"

    ```cpp
    #include "flox/backtest/backtest_runner.h"
    #include "flox/replay/abstract_event_reader.h"

    int main() {
      replay::ReaderFilter filter;
      filter.symbols = {1};
      auto reader = replay::createMultiSegmentReader("/data/btcusdt", filter);

      BacktestConfig config{ .initialCapital = 10000.0, .feeRate = 0.0004 };
      BacktestRunner runner(config);

      SymbolRegistry registry;
      SymbolInfo info{ .exchange = "binance", .symbol = "BTCUSDT",
                        .tickSize = Price::fromDouble(0.01) };
      registry.registerSymbol(info);

      SmaCrossover strat(1, 10, 20, Quantity::fromDouble(1.0), registry);
      runner.setStrategy(&strat);

      auto result = runner.run(*reader);
      auto stats = result.computeStats();
      std::cout << "Final " << stats.finalCapital
                << "  return " << stats.returnPct << "%\n";
    }
    ```

## Output

```
Final $10245.30  return 2.45%  trades 47  Sharpe 1.23  DD 3.21%
```

## Pre-aggregated bars (faster replay)

For repeated parameter sweeps over the same data, pre-aggregate bars once and replay them. The Python and Node.js bindings expose this via `BacktestRunner.run_bars(...)` (closed OHLC bars), and C++ has dedicated mmap storage.

=== "Python"

    ```python
    import numpy as np
    bt.run_bars(
        start_time_ns = ts_start_arr.astype(np.int64),
        end_time_ns   = ts_end_arr.astype(np.int64),
        open  = opens,  high = highs, low = lows, close = closes, volume = vols,
        symbol = "BTCUSDT",
    )
    ```

=== "Node.js"

    ```javascript
    bt.runBars(startNs, endNs, opens, highs, lows, closes, vols, "BTCUSDT");
    ```

=== "C++"

    Pre-aggregate offline with the `preagg_bars` tool, then replay via `MmapBarReplaySource`:

    ```cpp
    MmapBarStorage storage("/data/BTCUSDT/bars");
    MmapBarReplaySource source(storage, symbol_id);
    source.replay([&](const BarEvent& ev) { strat.onBar(ev); });
    ```

## See Also

- [Grid Search Optimization](grid-search.md) — parameter optimization
- [Bar Aggregation Pipeline](bar-aggregation.md) — pre-aggregating bars
- [Interactive Mode](interactive-backtest.md) — step-by-step execution
- [Realistic fills](backtest-realistic-fills.md) — slippage and queue position
