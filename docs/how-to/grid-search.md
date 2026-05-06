# Parameter optimization with grid search

Find good strategy parameters by sweeping a grid and ranking results. The pattern is the same in every binding: define a grid, run a backtest per combination, rank by your chosen metric.

## Quick start

=== "Python"

    ```python
    import flox_py as flox
    import pandas as pd

    def run_one(fast: int, slow: int) -> dict:
        reg = flox.SymbolRegistry()
        btc = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
        strat = SmaCrossover([btc], fast=fast, slow=slow)
        bt = flox.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000)
        bt.set_strategy(strat)
        return bt.run_csv("data/btcusdt_1m.csv", "BTCUSDT")

    rows = []
    for fast in [5, 10, 15, 20]:
        for slow in [20, 30, 40, 50]:
            if fast >= slow:
                continue
            stats = run_one(fast, slow)
            rows.append({"fast": fast, "slow": slow, **stats})

    df = pd.DataFrame(rows).sort_values("sharpe", ascending=False)
    print(df.head())
    df.to_csv("grid_results.csv", index=False)
    ```

    For parallelism, wrap `run_one` in `multiprocessing.Pool` or `concurrent.futures.ProcessPoolExecutor`. FLOX releases the GIL during the C++ backtest, so threads work too.

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');

    function runOne(fast, slow) {
      const reg = new flox.SymbolRegistry();
      const btc = reg.addSymbol("binance", "BTCUSDT", 0.01);
      const strat = new SmaCrossover([btc], fast, slow);
      const bt = new flox.BacktestRunner(reg, 0.0004, 10_000);
      bt.setStrategy(strat);
      return bt.runCsv("data/btcusdt_1m.csv", "BTCUSDT");
    }

    const rows = [];
    for (const fast of [5, 10, 15, 20]) {
      for (const slow of [20, 30, 40, 50]) {
        if (fast >= slow) continue;
        rows.push({ fast, slow, ...runOne(fast, slow) });
      }
    }
    rows.sort((a, b) => b.sharpeRatio - a.sharpeRatio);
    console.log(rows.slice(0, 5));
    ```

=== "C++"

    ```cpp
    #include "flox/backtest/backtest_optimizer.h"
    #include "flox/backtest/optimization_stats.h"

    struct MAParams {
      int fastPeriod, slowPeriod;
      std::string toString() const {
        return "fast=" + std::to_string(fastPeriod) + ",slow=" + std::to_string(slowPeriod);
      }
    };

    struct MAGrid {
      std::vector<int> fastPeriods = {5, 10, 15, 20};
      std::vector<int> slowPeriods = {20, 30, 40, 50};
      size_t totalCombinations() const { return fastPeriods.size() * slowPeriods.size(); }
      MAParams operator[](size_t i) const {
        return { fastPeriods[i / slowPeriods.size()], slowPeriods[i % slowPeriods.size()] };
      }
    };

    BacktestOptimizer<MAParams, MAGrid> optimizer;
    optimizer.setParameterGrid(MAGrid{});
    optimizer.setBacktestFactory([&](const MAParams& p) { return runBacktest(p); });
    auto results = optimizer.runLocal();
    auto ranked  = BacktestOptimizer<MAParams, MAGrid>::rankResults(results, RankMetric::SharpeRatio);
    std::cout << "Best: " << ranked[0].parameters.toString() << "\n";
    ```

The C++ optimizer parallelises across threads automatically (`runLocal(numThreads)`).

## Ranking metrics

| Metric | Python (in stats dict) | C++ (`RankMetric::*`) |
|---|---|---|
| Sharpe | `sharpe` / `sharpeRatio` | `SharpeRatio` |
| Sortino | `sortino` | `SortinoRatio` |
| Calmar | n/a (compute as `return / max_dd`) | `CalmarRatio` |
| Total return | `return_pct` | `TotalReturn` |
| Max drawdown | `max_drawdown_pct` | `MaxDrawdown` |
| Win rate | `win_rate` | `WinRate` |
| Profit factor | `profit_factor` | `ProfitFactor` |

## Filtering, stability, statistical tests

The C++ `BacktestOptimizer` ships ranking, filtering, bootstrap CIs, and permutation tests as templated utilities (see [`optimization_stats.h`](../reference/api/backtest/optimization_stats.md)). From Python / Node.js you use `pandas` / `numpy` / `scipy` directly:

=== "Python"

    ```python
    import numpy as np
    from scipy import stats

    sharpes = df["sharpe"].values
    returns = df["return_pct"].values
    print("mean Sharpe:", sharpes.mean(), " std:", sharpes.std())
    print("corr(Sharpe, return):", np.corrcoef(sharpes, returns)[0, 1])

    # Bootstrap 95% CI for mean Sharpe
    boot = [np.random.choice(sharpes, size=len(sharpes), replace=True).mean() for _ in range(10_000)]
    print("CI:", np.percentile(boot, [2.5, 97.5]))

    # Permutation test: top-10 vs bottom-10
    top, bot = np.sort(sharpes)[-10:], np.sort(sharpes)[:10]
    perm = stats.permutation_test((top, bot), lambda a, b: a.mean() - b.mean(),
                                    n_resamples=10_000, alternative="greater")
    print("p =", perm.pvalue)
    ```

=== "C++"

    ```cpp
    using Stats = OptimizationStatistics<MAParams, MAGrid>;
    Stats::printSummary(results);
    auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
    auto ci      = Stats::bootstrapCI(sharpes, 0.95, 10000);     // .lower / .median / .upper
    auto pValue  = Stats::permutationTest(group1, group2, 10000);
    Stats::generateReport(results, "report.md");
    ```

## Best practices

1. **Avoid overfitting** — every extra parameter increases overfit risk
2. **Walk-forward** — optimise on train, validate on a held-out test slice
3. **Parameter stability** — the best point should have good neighbours, not be an isolated peak
4. **Realistic costs** — include slippage and exchange fees
5. **Statistical significance** — bootstrap CI / permutation tests separate luck from edge

## Type-erased `GridSearch` class (Python / Node / Codon)

The template-based `BacktestOptimizer<ParamsT, GridT>` shown above is C++-only. For the language bindings, FLOX exposes a type-erased `GridSearch` class that takes axes of `double` values and a factory callback. Last axis varies fastest (row-major flatten).

### Python

```python
import flox_py as flox

reg = flox.SymbolRegistry()
btc = reg.add_symbol("exchange", "BTCUSDT", 0.01)


def factory(params):
    fast, slow = int(params[0]), int(params[1])
    if fast >= slow:
        return {"sharpe": 0.0, "return_pct": 0.0, "total_trades": 0}

    class _S(flox.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.fast = flox.SMA(fast)
            self.slow = flox.SMA(slow)
        def on_trade(self, ctx, t):
            f = self.fast.update(t.price); s = self.slow.update(t.price)
            if f is None or s is None or not self.slow.ready: return
            if f > s and ctx.is_flat(): self.market_buy(0.01)
            elif f < s and ctx.is_flat(): self.market_sell(0.01)

    bt = flox.BacktestRunner(reg, 0.0004, 10_000)
    bt.set_strategy(_S([btc]))
    return bt.run_csv("data/btcusdt_sample.csv", symbol="BTCUSDT")


gs = flox.GridSearch()
gs.add_axis([5.0, 10.0, 20.0])
gs.add_axis([30.0, 50.0, 100.0])
gs.set_factory(factory)
for r in gs.run():
    fast, slow = r["params"]
    s = r["stats"]
    print(f"fast={int(fast):3d} slow={int(slow):3d}: "
          f"return={s['return_pct']:+.4f}% sharpe={s['sharpe']:+.4f}")
```

The factory takes `list[float]` and returns the same dict shape as `BacktestRunner.run_csv`.

### Node

```js
const gs = new flox.GridSearch();
gs.addAxis([5, 10, 20]);
gs.addAxis([30, 50, 100]);
gs.setFactory((params) => {
  // ... return camelCase BacktestStats: { returnPct, sharpeRatio, totalTrades, ... }
});
const results = gs.run();  // [{index, params, stats}, ...]
```

### Pairs with walk-forward

Compose with [walk-forward](walk-forward.md): run a `GridSearch` on each fold's train slice, pick the best params by your criterion, evaluate on test. The two primitives stay separate so the choice of "best" stays opinionated to the caller (sharpe vs. profit factor vs. drawdown-aware).

### Limitations

The current backend runs combinations sequentially. Multi-process / Ray Tune backends are W6-T002 / W6-T003 follow-ups. Result list is unsorted — sort or filter on the caller side.

## See also

- [Backtesting](./backtest.md)
- [Walk-forward](./walk-forward.md)
- [Optimization stats reference (C++)](../reference/api/backtest/optimization_stats.md)
- [Python optimizer reference](../reference/python/optimizer.md)
