# Realistic backtest fills

Configure a backtest with realistic execution: slippage on market orders, queue simulation for limit orders, and an exported equity curve. Same model in every binding.

## 1. Configure the simulator

=== "Python"

    ```python
    import flox_py as flox

    ex = flox.SimulatedExecutor()
    ex.set_default_slippage("fixed_bps", bps=1.0)   # 1 bps default
    ex.set_queue_model("tob")                       # top-of-book queue
    # Per-symbol override:
    ex.set_symbol_slippage(eth_usd, "volume_impact", impact_coeff=0.01)
    ```

=== "Node.js"

    ```javascript
    const ex = new flox.SimulatedExecutor();
    ex.setDefaultSlippage("fixed_bps", 0, 0, 1.0, 0);    // ticks, tickSize, bps, impactCoeff
    ex.setQueueModel("tob", 1);
    ```

=== "Codon"

    ```python
    from flox.backtest import SimulatedExecutor, SLIPPAGE_FIXED_BPS, QUEUE_TOB

    ex = SimulatedExecutor()
    ex.set_default_slippage(SLIPPAGE_FIXED_BPS, 0, 0.0, 1.0, 0.0)
    ex.set_queue_model(QUEUE_TOB, 1)
    ```

=== "C++"

    ```cpp
    #include "flox/backtest/backtest_config.h"
    #include "flox/backtest/backtest_runner.h"

    flox::BacktestConfig cfg;
    cfg.initialCapital = 100'000.0;
    cfg.feeRate = 0.0002;                                                                 // 2 bps per fill
    cfg.defaultSlippage = { flox::SlippageModel::FIXED_BPS, 0, flox::Price{}, 1.0, 0.0 };  // 1 bps default
    cfg.queueModel = flox::QueueModel::TOB;
    cfg.riskFreeRate = 0.0;
    cfg.metricsAnnualizationFactor = 252.0;

    flox::BacktestRunner runner(cfg);

    cfg.perSymbolSlippage.emplace_back(
        kEthUsd, flox::SlippageProfile{flox::SlippageModel::VOLUME_IMPACT, 0, 0.0, 0.01});
    ```

## 2. Run it

=== "Python"

    ```python
    bt = flox.BacktestRunner(reg, fee_rate=0.0002, initial_capital=100_000)
    bt.set_strategy(my_strategy)
    stats = bt.run_csv("data.csv", "BTCUSDT")
    ```

=== "Node.js"

    ```javascript
    const bt = new flox.BacktestRunner(reg, 0.0002, 100_000);
    bt.setStrategy(myStrategy);
    const stats = bt.runCsv("data.csv", "BTCUSDT");
    ```

=== "C++"

    ```cpp
    runner.setStrategy(&yourStrategy);
    auto result = runner.run(*reader);   // reader: replay::IMultiSegmentReader
    auto stats  = result.computeStats();
    ```

## 3. Inspect stats

These are the fields on the dict returned by `run_csv` / `run_ohlcv` / `run_bars` / `run_tape` / `run_tapes` (snake_case in Python/Codon, camelCase in Node, `BacktestStats` struct in C++).

| Python / Codon | Node.js | Description |
|---|---|---|
| `total_trades` | `totalTrades` | Number of closed trades |
| `net_pnl` | `netPnl` | Total P&L net of fees |
| `return_pct` | `returnPct` | Total return % |
| `sharpe_ratio` | `sharpeRatio` | Annualised Sharpe |
| `sortino_ratio` | `sortinoRatio` | Annualised Sortino |
| `calmar_ratio` | `calmarRatio` | Calmar ratio |
| `max_drawdown_pct` | `maxDrawdownPct` | Worst drawdown |
| `win_rate` | `winRate` | Win rate |
| `profit_factor` | `profitFactor` | Gross profit / gross loss |

`BacktestResult.stats()` is a *different* dict with a wider field set, but the same key names. See [Running a backtest](backtest.md#two-stats-shapes-one-set-of-key-names).

## 4. Export the equity curve

=== "Python"

    ```python
    curve = bt.equity_curve()   # dict of numpy arrays
    ts, eq, dd = curve["timestamp_ns"], curve["equity"], curve["drawdown_pct"]

    # BacktestRunner has no CSV writer. write_equity_curve_csv lives on
    # BacktestResult, which you drive from a SimulatedExecutor:
    res = flox.BacktestResult(initial_capital=100_000.0, fee_rate=0.0002)
    res.ingest_executor(ex)
    res.write_equity_curve_csv("equity.csv")   # or res.equity_curve() -> structured array
    ```

=== "Node.js"

    ```javascript
    const curve = bt.equityCurve();   // { timestampNs, equity, drawdownPct }

    // Node has no equity-curve CSV writer on either BacktestRunner or
    // BacktestResult. Write the rows yourself from the arrays above.
    ```

=== "C++"

    ```cpp
    for (const auto& pt : result.equityCurve()) {
      fmt::print("{},{:.2f},{:.2f}\n", pt.timestampNs, pt.equity, pt.drawdownPct);
    }
    result.writeEquityCurveCsv("equity.csv");
    ```

CSV header: `timestamp_ns,equity,drawdown_pct`. One row per closed trade.

## See also

- [Backtesting](backtest.md)
- [Slippage reference](../reference/api/backtest/slippage.md)
- [Queue simulation](../reference/api/backtest/queue_simulation.md)
