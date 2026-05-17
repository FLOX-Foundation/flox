## Walk-forward backtesting

Walk-forward splits the time series into successive train / test
windows, runs the strategy through each pair, and returns per-fold
stats. Useful as a sanity check on overfitting: if the strategy looks
great on the train half but degrades on the held-out test half across
folds, the backtest is overfit.

`flox_py.WalkForwardRunner` ships in Python. The same primitive is in
`flox.WalkForwardRunner` for Node and through the C ABI for Codon
(`flox_walk_forward_run_csv`).

## Modes

`anchored` — the train window starts at bar 0 and grows. Each fold
trains on `[0, t]`, tests on `[t, t + test_size]`, then `t` advances by
`step`. Set `min_train_size` to skip the first folds where the train
window is too small.

`sliding` — the train window is fixed-size and slides forward. Each
fold trains on `[t, t + train_size]`, tests on
`[t + train_size, t + train_size + test_size]`, `t` advances by `step`.

## Python

```python
import flox_py as flox

reg = flox.SymbolRegistry()
btc = reg.add_symbol("exchange", "BTCUSDT", 0.01)


class SmaCross(flox.Strategy):
    def __init__(self, syms):
        super().__init__(syms)
        self.fast = flox.SMA(10)
        self.slow = flox.SMA(30)

    def on_trade(self, ctx, t):
        f = self.fast.update(t.price)
        s = self.slow.update(t.price)
        if f is None or s is None or not self.slow.ready:
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.01)
        elif f < s and ctx.is_flat():
            self.market_sell(0.01)


wfr = flox.WalkForwardRunner(
    reg, fee_rate=0.0004, initial_capital=10_000,
    mode="anchored", test_size=100, step=100, min_train_size=100,
)

# Factory called twice per fold (train, then test). Build a fresh
# strategy every time — state from a prior fold must not leak.
wfr.set_strategy_factory(lambda fold_index: SmaCross([btc]))

folds = wfr.run_csv("data/btcusdt_sample.csv", "BTCUSDT")
for f in folds:
    print(f"fold {f['fold_index']}: "
          f"train return={f['train_stats']['return_pct']:+.4f}% "
          f"sharpe={f['train_stats']['sharpe']:+.4f} | "
          f"test return={f['test_stats']['return_pct']:+.4f}% "
          f"sharpe={f['test_stats']['sharpe']:+.4f}")
```

The factory pattern is non-negotiable: the engine calls it once per
window with no shortcut for "reuse my strategy". A leaked indicator
buffer or position counter from a prior fold would silently corrupt
the next fold's stats.

## Node

```js
const flox = require('flox-node');

const reg = new flox.SymbolRegistry();
const btc = reg.addSymbol('exchange', 'BTCUSDT', 0.01);

const wfr = new flox.WalkForwardRunner(reg, 0.0004, 10000, {
  mode: 'anchored', testSize: 100, step: 100, minTrainSize: 100,
});

wfr.setStrategyFactory((foldIndex) => {
  const fast = new flox.SMA(10);
  const slow = new flox.SMA(30);
  return {
    symbols: [Number(btc)],
    onTrade(ctx, t, emit) {
      const f = fast.update(t.price);
      const s = slow.update(t.price);
      if (f === null || s === null || !slow.ready) return;
      if (f > s && ctx.position === 0) emit.marketBuy(0.01);
      else if (f < s && ctx.position === 0) emit.marketSell(0.01);
    },
  };
});

const folds = wfr.runCsv('data/btcusdt_sample.csv', 'BTCUSDT');
folds.forEach(f => console.log(f.foldIndex, f.testStats.returnPct));
```

## What you get back per fold

```
{
  "fold_index": 0,
  "train_start_bar": 0, "train_end_bar": 100,
  "test_start_bar": 100, "test_end_bar": 200,
  "train_start_ns": ..., "train_end_ns": ...,
  "test_start_ns": ..., "test_end_ns": ...,
  "train_stats": { ... full BacktestStats ... },
  "test_stats":  { ... full BacktestStats ... },
}
```

The two `*_stats` blocks are the same shape as `BacktestRunner.run_csv`
returns. Compute aggregate statistics (mean / median / variance over
folds) on the client side — the runner does not aggregate for you on
purpose, since useful aggregates depend on what you are looking for
(robustness vs. average performance vs. worst case).

## Full OHLCV path for `on_bar` strategies

`run_csv` replays close prices as synthetic trade events — `on_trade`
fires, but `on_bar` does not, and intrabar high / low / volume are
not preserved. For strategies whose decisions depend on bar internals
(TP/SL ladders on high/low, ATR-style indicators, breakout filters),
use `run_bars` with numpy arrays:

```python
import flox_py as flox
import numpy as np

reg = flox.SymbolRegistry()
btc = reg.add_symbol("exchange", "BTCUSDT", 0.01)


class IntrabarBreakout(flox.Strategy):
    def __init__(self, syms):
        super().__init__(syms)
        self.in_pos = False

    def on_bar(self, ctx, bar):
        if not self.in_pos and bar.high >= bar.open * 1.005:
            self.market_buy(0.01)
            self.in_pos = True
        elif self.in_pos and bar.low <= bar.open * 0.99:
            self.market_sell(0.01)
            self.in_pos = False


wfr = flox.WalkForwardRunner(
    reg, fee_rate=0.0, initial_capital=10_000,
    mode="sliding", train_size=4380, test_size=2190, step=2190,
)
wfr.set_strategy_factory(lambda _i: IntrabarBreakout([btc]))

# OHLCV arrays — all must have the same length, sorted by end_time_ns.
start_ns = ...  # int64 ns, bar open
end_ns = ...    # int64 ns, bar close
open_, high, low, close, volume = ...  # float64

folds = wfr.run_bars(
    start_ns, end_ns, open_, high, low, close, volume,
    symbol="BTCUSDT")
```

Each fold dispatches `BarEvent`s with full OHLCV preserved. `on_bar`
fires; `on_trade` does not — same convention as
`BacktestRunner.run_bars`. `bar_type` (default `0` = Time) and
`bar_type_param` are forwarded for non-time bar aggregations.

## What walk-forward does not do

It does not optimise hyperparameters per fold. If you need that, run
[grid search](grid-search.md) on each fold's train slice yourself,
pick the best params, then evaluate on test. That pattern is the
standard "walk-forward optimisation" but it is opinionated enough that
the runner stays out of it — compose the primitives.
