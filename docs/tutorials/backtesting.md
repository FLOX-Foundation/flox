# Backtesting

Run your strategy against recorded market data.

## Prerequisites

- Completed [Recording Data](recording-data.md) (or have a CSV / `.floxlog` file)
- Build / install with backtest support — see [Bindings](../bindings/README.md) for per-language details

## The pipeline

```
data file → BacktestRunner → Strategy → SimulatedExecutor → BacktestResult
```

`BacktestRunner` runs your strategy through a `SimulatedExecutor`
with a flat fee rate and nothing else. Useful for indicator sanity
checks. Numbers it produces ignore funding, liquidation, queue
position, rate limits, and venue outages — the forces that decide
whether a perp strategy survives in production.

Venue physics live in a separate simulation, `VenueStack`, which you
drive directly rather than plugging into `BacktestRunner`. See
[Realistic backtest in one call](../how-to/realistic-backtest.md)
and [Cross-margin accounts](../how-to/cross-margin.md).

## Minimal example

A strategy that buys when price crosses above a 20-period SMA. Match
the output against a known baseline; do not size positions off these
numbers.

=== "Python"

    ```python
    import flox_py as flox
    from collections import deque

    class CrossAboveSMA(flox.Strategy):
        def __init__(self, symbols, period=20):
            super().__init__(symbols)
            self.window = deque(maxlen=period)

        def on_trade(self, ctx, trade):
            self.window.append(trade.price)
            if len(self.window) < self.window.maxlen:
                return
            sma = sum(self.window) / len(self.window)
            if trade.price > sma and ctx.is_flat():
                self.market_buy(1.0)
            elif trade.price < sma and ctx.is_long():
                self.close_position()

    reg = flox.SymbolRegistry()
    btc = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)

    strat = CrossAboveSMA([btc])
    bt = flox.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000)
    bt.set_strategy(strat)

    stats = bt.run_csv("/data/btcusdt_1m.csv", "BTCUSDT")
    print(f"Return {stats['return_pct']:.2f}%  Sharpe {stats['sharpe']:.2f}  "
          f"DD {stats['max_drawdown_pct']:.2f}%  trades {stats['total_trades']}")
    ```

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');

    class CrossAboveSMA {
      constructor(symbols, period = 20) {
        this.symbols = symbols;
        this.period = period;
        this.window = [];
      }
      onTrade(ctx, trade, emit) {
        this.window.push(trade.price);
        if (this.window.length > this.period) this.window.shift();
        if (this.window.length < this.period) return;
        const sma = this.window.reduce((a,b)=>a+b, 0) / this.period;
        if (trade.price > sma && ctx.position === 0) emit.marketBuy(1.0);
        else if (trade.price < sma && ctx.position > 0) emit.closePosition();
      }
    }

    const reg = new flox.SymbolRegistry();
    const btc = reg.addSymbol("binance", "BTCUSDT", 0.01);
    const bt  = new flox.BacktestRunner(reg, 0.0004, 10_000);
    bt.setStrategy(new CrossAboveSMA([btc]));

    const stats = bt.runCsv("/data/btcusdt_1m.csv", "BTCUSDT");
    console.log(`Return ${stats.returnPct.toFixed(2)}%  Sharpe ${stats.sharpeRatio.toFixed(2)}`);
    ```

=== "Codon"

    ```python
    from flox.runner import BacktestRunner
    from flox.strategy import Strategy
    from flox.context import SymbolContext
    from flox.types import TradeData
    from flox.indicators import SMA

    class CrossAboveSMA(Strategy):
        sma: SMA
        def __init__(self, symbols: List[int], period: int = 20):
            super().__init__(symbols)
            self.sma = SMA(period)

        def on_trade(self, ctx: SymbolContext, trade: TradeData):
            v = self.sma.update(trade.price.to_double())
            if not self.sma.ready:
                return
            if trade.price.to_double() > v and self.position() == 0.0:
                self.market_buy(1.0)
            elif trade.price.to_double() < v and self.position() > 0.0:
                self.close_position()

    bt = BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(CrossAboveSMA([btc]))
    stats = bt.run_csv("/data/btcusdt_1m.csv", "BTCUSDT")
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
      auto symId = registry.registerSymbol(info);

      MyStrategy strat(symId, registry);
      runner.setStrategy(&strat);

      auto result = runner.run(*reader);
      auto stats  = result.computeStats();
      std::cout << "Return " << stats.returnPct << "%\n";
    }
    ```

## What's in `stats`

| Field | Description |
|---|---|
Python and Codon use snake_case, Node.js camelCase. C++ reads the
same fields off `BacktestStats`.

| Python / Codon | Node.js | Description |
|---|---|---|
| `total_trades` | `totalTrades` | Number of closed trades |
| `final_capital` | `finalCapital` | Ending capital |
| `return_pct` | `returnPct` | Total return % |
| `sharpe` | `sharpeRatio` | Annualised Sharpe |
| `sortino` | (absent) | Annualised Sortino |
| `max_drawdown_pct` | `maxDrawdownPct` | Worst drawdown |
| `win_rate` | `winRate` | Win rate (0–1) |
| `profit_factor` | `profitFactor` | Gross profit / gross loss |

Python `BacktestRunner.run_*` returns a plain dict; Codon returns a
`BacktestStats` object with attribute access; Node.js returns an object
literal.

Sharpe is the one key whose name does not follow the snake_case to
camelCase rule, and the shapes differ per producer:

| Producer | Sharpe key | Sortino / Calmar |
|---|---|---|
| Python `BacktestRunner.run_*` | `sharpe` | `sortino`; no calmar (`python/strategy_bindings.h:2113-2114`) |
| Python `BacktestResult.stats()` | `sharpe_ratio` | `sortino_ratio`, `calmar_ratio` (`python/backtest_bindings.h:613-615`) |
| Node `BacktestRunner.runCsv` / `runOhlcv` / `runBars` | `sharpeRatio` | neither is emitted (`node/src/strategy.h:1527-1546`) |
| Node `BacktestResult.stats()` | `sharpe` | `sortino`, `calmar` (`node/src/backtest.h:578-580`) |
| Node `GridSearch` / `WalkForwardRunner` | `sharpeRatio` | `sortinoRatio` (`node/src/walk_forward.h:77-78`) |
| Codon `run_*` | `sharpe` | `sortino`, `calmar` (`codon/flox/runner.codon:343-345`) |

## Time-range filtering

=== "Python"

    Use `pandas` to slice your CSV before passing in, or pass arrays to `run_bars(start_time_ns, end_time_ns, ...)` filtered to the window you want.

=== "C++"

    ```cpp
    replay::ReaderFilter filter;
    filter.from_ns = 1704067200000000000LL;   // 2024-01-01
    filter.to_ns   = 1704153600000000000LL;   // 2024-01-02
    filter.symbols = {1};
    auto reader = replay::createMultiSegmentReader("/data", filter);
    ```

## Performance tips

1. **Release build** — `cmake -DCMAKE_BUILD_TYPE=Release` (`pip install flox-py` already gives you Release)
2. **Filter symbols** — only load what you need
3. **Pre-aggregate** for repeated parameter sweeps; see [Bar aggregation](../how-to/bar-aggregation.md)
4. **Avoid logging in callbacks** — measure first; logging in the inner loop dominates

## Next

`BacktestRunner` is a flat fee rate, no funding, no liquidation, no
rate limits. Good enough for a sanity check; not enough before live.
`flox.VenueStack.binance_um_futures(account_id=42, equity=10_000)`
gives you an executor, account, liquidation engine, fee schedule and
funding schedule with venue defaults, driven directly. See the links
below.

- [Realistic backtest in one call](../how-to/realistic-backtest.md) — venue stack
- [Cross-margin accounts](../how-to/cross-margin.md) — share equity across positions
- [Liquidation and ADL](../how-to/liquidation-and-adl.md) — cascade behaviour
- [Paper trading](../how-to/paper-trading.md) — same strategy class, live feed
- [Connect FLOX to a CCXT exchange](../how-to/ccxt-adapter.md) — promote to live
- [Inspect a tape and run in the replay viewer](../how-to/replay-viewer.md)
- [Running a Backtest](../how-to/backtest.md) — fuller SMA crossover walkthrough
- [Grid search](../how-to/grid-search.md) — sweep parameters
- [Realistic fills](../how-to/backtest-realistic-fills.md) — slippage and queue position
- [Bar aggregation](../how-to/bar-aggregation.md) — pre-aggregate offline for speed
