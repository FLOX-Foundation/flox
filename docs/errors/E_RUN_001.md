---
code: E_RUN_001
title: Backtest run before set_strategy()
severity: error
since: 0.5.7
---

# E_RUN_001 — Backtest run before `set_strategy()`

`BacktestRunner.run_*()` was called without first attaching a strategy
via `set_strategy()`. The runner needs a strategy to know what to do
with each bar.

## How to fix

=== "Python"
    ```python
    import flox

    runner = flox.BacktestRunner(reg, fee_rate=0.001, initial_capital=10_000)
    runner.set_strategy(MyStrategy())          # ← required first
    runner.run_csv("data.csv")
    ```

=== "Node.js"
    ```js
    const runner = new flox.BacktestRunner(reg, 0.001, 10000);
    runner.setStrategy({ /* ... */ });          // ← required first
    runner.runCsv("data.csv", "BTC");
    ```

## Common causes

- The strategy class was instantiated but never passed to `set_strategy()`.
- Code was refactored and the `set_strategy()` call accidentally removed.
