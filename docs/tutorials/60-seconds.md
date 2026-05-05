# 60 seconds to your first backtest

This tutorial gets you from zero to a running SMA crossover backtest in
under a minute. Four commands, no C++ build, no boilerplate.

## Prerequisites

- Python 3.10+
- `pip` on `PATH`

## 1. Install (15 s)

```bash
pip install flox-py
```

`flox-py` ships a precompiled C++ engine plus the `flox` console script.

## 2. Scaffold a project (5 s)

```bash
flox new my-strategy
cd my-strategy
```

`flox new` creates a directory with `main.py`, `requirements.txt`, and
a README. The default template is `research`: a single-file SMA(10/30)
crossover on BTCUSDT.

## 3. Install template deps (10 s)

```bash
pip install -r requirements.txt
```

Just `flox-py` and `numpy`.

## 4. Run it (5 s)

```bash
python main.py
```

You should see:

```
  (set MY_STRATEGY_DATA=<csv> for backtest mode)
-- synthetic live ------------------------------------------
  my-strategy strategy started
  my-strategy stopped  (31 signals)
  signals: 31
```

That's a live-mode run against a synthetic price path — handy for the
very first sanity check. To switch to a real backtest, point the env
var at a CSV with columns `timestamp_ms,price,qty,is_buyer_maker`:

```bash
MY_STRATEGY_DATA=/path/to/btcusdt_1m.csv python main.py
```

…and you'll get an actual return / Sharpe / drawdown report:

```
-- backtest -------------------------------------------------
  return : +1.2340%
  trades : 42  win=54.8%
  sharpe : 1.4321
  max DD : -3.2100%
```

Total elapsed: **~35 seconds** if the `pip install` is cached, **~60
seconds** on a clean environment.

## What just happened?

`main.py` is a single file you edit:

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

The same class drives both the synthetic live run and the CSV backtest
because the engine model is identical in both modes — see
[strategy classes](../how-to/strategy-classes.md) for why.

## Next steps

- Replace the SMA crossover with your own indicators (see
  [Indicator Graph](../how-to/indicator-graph.md)).
- Run a parameter sweep with [Grid Search](../how-to/grid-search.md).
- Wire a live exchange feed using
  [`docs/examples/python_ccxt_live.py`](../examples/python-ccxt-live.md).
- Read the [Python quickstart](python-quickstart.md) for the
  longer, hand-built version of the same flow.
