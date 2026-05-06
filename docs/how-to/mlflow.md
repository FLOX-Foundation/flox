# Log a backtest to MLflow

`flox_py.mlflow` writes FLOX backtest output into [MLflow](https://mlflow.org)
runs so they show up in the MLflow UI. `mlflow` is an optional
dependency:

```bash
pip install mlflow
```

Python-only for now. Node / Codon backtests can produce the same
JSON stats; if you want them in MLflow, route them through any
MLflow client (REST API, language SDK).

## One-shot logging

Run a backtest, then log it.

```python
import flox_py as flox
from flox_py import mlflow as flox_mlflow

registry = flox.SymbolRegistry()
btc = registry.add_symbol("exchange", "BTCUSDT", tick_size=0.01)

bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(MyStrategy([btc]))
stats = bt.run_csv("btcusdt_1m.csv", symbol="BTCUSDT")

run_id = flox_mlflow.log_backtest(
    stats=stats,
    equity_curve=bt.equity_curve(),
    trades=bt.trades(),
    params={"fast": 10, "slow": 30, "fee": 0.0004},
    run_name="sma-crossover-2025-01",
    experiment="my-strategy",
)
print(f"logged as run {run_id}")
```

What ends up where:

| Source | MLflow field |
|---|---|
| Numeric `stats` keys (`return_pct`, `sharpe`, `max_drawdown_pct`, `total_trades`, `win_rate`, …) | `metrics` |
| Non-numeric `stats` keys (timestamps, names) | `tags` |
| `params` | `params` |
| `equity_curve` | `equity_curve.csv` artifact, plus `equity_curve.png` when matplotlib is installed |
| `trades` | `trades.csv` artifact |
| `html_report` (path to a file) | the file attached as an artifact |
| `tags` | run tags |

NaN and Inf are not valid MLflow metrics. The integration records
them as tag strings instead so the value is still visible in the UI.

## Context-manager flow

`log_to_mlflow` opens a run, yields a logger, and closes the run on
block exit. Use it when you want extra params, tags, or artifacts in
the same run.

```python
with flox_mlflow.log_to_mlflow(
    run_name="grid-cell-fast=10-slow=30",
    experiment="my-strategy",
) as run:
    run.log_params({"fast": 10, "slow": 30})
    run.log_tags({"data": "btcusdt_1m_2024Q4"})
    stats = bt.run_csv("btcusdt_1m.csv", symbol="BTCUSDT")
    run.log_backtest(
        stats=stats,
        equity_curve=bt.equity_curve(),
        trades=bt.trades(),
    )
    run.log_artifact("debug_plot.png")
```

The yielded `run.run_id` is the MLflow run ID — pass it on if you
need to nest other work under this run.

## Pointing at a tracking server

The default tracking URI is whatever `mlflow.get_tracking_uri()`
already returns, which honours the `MLFLOW_TRACKING_URI` env var and
any prior `mlflow.set_tracking_uri(...)` call.

Inline override:

```python
flox_mlflow.log_backtest(
    stats=stats,
    tracking_uri="http://localhost:5000",
    experiment="my-strategy",
)
```

Local file backend (no server):

```bash
mlflow ui --backend-store-uri ./mlruns
```

Then either pass `tracking_uri="file:./mlruns"` to the call or
`export MLFLOW_TRACKING_URI=file:./mlruns` before running the script.
The UI at <http://localhost:5000> picks the runs up.

## Use it inside a grid search

`GridSearch` returns one stats dict per parameter cell. Wrap the
sweep in a parent run and each cell in a nested child run:

```python
import flox_py as flox
from flox_py import mlflow as flox_mlflow

with flox_mlflow.log_to_mlflow(run_name="grid-2025-01",
                               experiment="my-strategy") as parent:
    parent.log_params({"data": "btcusdt_1m_2024Q4"})

    results = grid.run()  # list of {params, stats}
    for cell in results:
        with flox_mlflow.log_to_mlflow(
            run_name=f"cell-{cell['params']}",
            experiment="my-strategy",
            nested=True,
        ) as child:
            child.log_backtest(
                stats=cell["stats"],
                params=cell["params"],
            )
```

The UI groups the children under the parent, so a 64-cell grid is
one row with a chevron instead of 64 separate rows.
