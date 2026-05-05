# 60 seconds to your first backtest

This walks through getting a backtest running starting from a clean
Python install. Four commands.

## Prerequisites

- Python 3.10+
- `pip` on `PATH`

## 1. Install

```bash
pip install flox-py
```

The wheel includes the compiled C++ engine and a `flox` console script.

## 2. Scaffold a project

```bash
flox new my-strategy
cd my-strategy
```

This writes `main.py`, `requirements.txt`, and a README. The default
template is `research`: a single-file SMA(10/30) crossover on BTCUSDT.

## 3. Install template deps

```bash
pip install -r requirements.txt
```

`flox-py` and `numpy`.

## 4. Run it

```bash
python main.py
```

Output:

```
  (set MY_STRATEGY_DATA=<csv> for backtest mode)
-- synthetic live ------------------------------------------
  my-strategy strategy started
  my-strategy stopped  (31 signals)
  signals: 31
```

That run uses a synthetic price path — there is no executor wired, so
positions stay flat and the strategy keeps emitting buys. It's a
smoke test, not a strategy evaluation.

For a real backtest, point the env var at a CSV with columns
`timestamp_ms,price,qty,is_buyer_maker`:

```bash
MY_STRATEGY_DATA=/path/to/btcusdt_1m.csv python main.py
```

That branch runs through `BacktestRunner`, which has a simulated
executor, and prints a summary:

```
-- backtest -------------------------------------------------
  return : +1.2340%
  trades : 42  win=54.8%
  sharpe : 1.4321
  max DD : -3.2100%
```

## The strategy class

`main.py` is one file. The class you edit:

```python
class my_strategy_strategy(flox.Strategy):
    def on_trade(self, ctx, trade):
        fv = self.fast.update(trade.price)
        sv = self.slow.update(trade.price)
        if fv is None or sv is None or not self.slow.ready:
            return
        if fv > sv and ctx.is_flat():
            self.market_buy(0.01)
        elif fv < sv and ctx.is_flat():
            self.market_sell(0.01)
```

Same class runs under both `Runner` (the synthetic mode above) and
`BacktestRunner`. See [strategy classes](../how-to/strategy-classes.md).

## Next steps

- Replace the SMA crossover with your own indicators
  ([Indicator Graph](../how-to/indicator-graph.md)).
- Parameter sweep via [Grid Search](../how-to/grid-search.md).
- Wire a live exchange feed:
  [`docs/examples/python_ccxt_live.py`](../examples/python-ccxt-live.md).
- The longer hand-built version: [Python quickstart](python-quickstart.md).
