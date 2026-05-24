# Your first FLOX backtest

This tutorial gets you from a clean Python environment to a backtest
running on real BTC 1m data. Three commands.

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

`flox new` writes `main.py`, `requirements.txt`, a README, and a
`data/btcusdt_sample.csv` with 500 real BTC/USDT 1-minute bars
(`timestamp,open,high,low,close,volume`). The default template is
`research`: a single-file SMA(10/30) crossover.

## 3. Run

```bash
pip install -r requirements.txt
python main.py
```

Output:

```
backtest on btcusdt_sample.csv
  return : -1.2103%
  trades : 187  win=66.3%
  sharpe : -4.5746
  max DD : 1.5030%
  net PnL: -121.0296
```

That is the actual result of an SMA(10/30) crossover on 500 minutes of
BTC. 66% of trades closed in the green, but small wins do not pay for
fees and the few losers — net result is a 1.2% drawdown. Useful as a
sanity check and a baseline; also useful as a reminder that
"SMA crossover" is not a strategy.

!!! note "This is the unrealistic baseline"
    The `research` template wires a bare `BacktestRunner` with a flat
    fee rate and no funding, liquidation, rate limits, or queue
    position. Numbers come back, but they ignore most of what makes
    a real perp account lose money. For a backtest you can trust
    enough to risk capital against, switch to the venue-realistic
    stack: see [Realistic backtest in one call](../how-to/realistic-backtest.md).

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

## Use your own data

Set `MY_STRATEGY_DATA` (the env-var prefix is the upper-cased project
slug + `_DATA`) to a CSV with the same columns:

```bash
MY_STRATEGY_DATA=/path/to/your/btcusdt_1h.csv python main.py
```

## Next steps

- Go realistic: [`flox.VenueStack.binance_um_futures(...)`](../how-to/realistic-backtest.md)
  wires fees, funding, liquidation, rate limits, and queue position
  in one call.
- Promote to paper trading against a live feed:
  [Paper trading](../how-to/paper-trading.md).
- Promote to live via ccxt:
  [Connect FLOX to a CCXT exchange](../how-to/ccxt-adapter.md).
- Replace the SMA crossover with your own indicators
  ([Indicator Graph](../how-to/indicator-graph.md)).
- Parameter sweep via [Grid Search](../how-to/grid-search.md).
- Inspect a run visually:
  [Inspect a tape and run in the replay viewer](../how-to/replay-viewer.md).
- Longer hand-built version (without the scaffolder):
  [Python quickstart](python-quickstart.md).
