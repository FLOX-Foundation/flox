---
code: E_RUN_003
title: Invalid factory return value
severity: error
since: 0.5.7
---

# E_RUN_003 — Invalid factory return value

A factory callback passed to `GridSearch` (and similar grid-search
constructs) must return a stats dict (the same shape that
`BacktestRunner.run_csv` returns). Returning anything else — `None`,
a list, a non-dict mapping — fails fast.

## How to fix

=== "Python"

    ```python
    def factory(params):
        fast, slow = int(params[0]), int(params[1])
        if fast >= slow:
            # Skip invalid combinations by returning an empty / zero
            # stats dict (still a dict).
            return {"sharpe": 0.0, "return_pct": 0.0, "total_trades": 0}

        bt = flox.BacktestRunner(reg, 0.0004, 10_000)
        bt.set_strategy(MyStrategy(fast, slow))
        return bt.run_csv("data.csv", symbol="BTCUSDT")  # ← dict
    ```

The grid runner reads `factory(params)`, expects `dict`, builds the
result row, and moves on. If your factory throws, the exception
propagates out of `GridSearch.run()`.

## When this fires

- Factory returned `None` (forgot the `return`).
- Factory returned the `BacktestRunner` instance instead of `bt.run_csv(...)`.
- Factory returned a tuple / list / numpy record array.
