# Cross-symbol indicators

Compute an indicator that consumes **two** synchronised symbol
streams — `Correlation(BTC, ETH)`, lag-sweep `AutoCorrelation`
across pairs, hedge ratios, regime filters keyed off a benchmark.

> **Different page**: `multi-symbol-indicators.md` covers running
> the *same* single-input indicator (SMA, EMA, RSI) independently
> per symbol. This page covers two-input indicators where the
> output depends on values from **both** symbols at the same
> instant.

## Alignment is your job

The engine does **not** synchronise across symbols for you. Two
separate trade / bar streams arrive at different timestamps and
different cadences (BTCUSDT trades at one tick rate, ETHUSDT at
another). Before feeding a pair-input indicator you must:

1. **Pick a clock** — usually the slower symbol's bar boundaries.
2. **Forward-fill** missing observations on the faster symbol so
   each clock tick has a value for both.
3. **Drop warmup** — discard the first samples until both series
   have a value.

The `flox.FeedClock` helper exists exactly for step (1) on the
streaming path; see [`multi-feed-clock.md`](multi-feed-clock.md).

## Batch recipe (Python)

For research / backtest preprocessing, numpy's structured arrays
are the simplest path:

```python
import flox_py as flox
import numpy as np

# Two pre-aligned bar arrays — same length, same timestamps.
btc_close = btc_bars["close"]
eth_close = eth_bars["close"]
assert btc_close.shape == eth_close.shape

# Streaming Correlation with a 30-bar window.
corr = flox.Correlation(period=30)
out = np.empty(len(btc_close))
for i, (b, e) in enumerate(zip(btc_close, eth_close)):
    out[i] = corr.update(b, e) or float("nan")

# Final value for the trailing 30-bar window:
print(corr.value)
```

If your bar arrays are **not** pre-aligned, do it with a
timestamp join first:

```python
import pandas as pd

df = pd.merge_asof(
    btc_bars.sort_values("ts"),
    eth_bars.sort_values("ts"),
    on="ts", suffixes=("_btc", "_eth"),
    direction="backward",
)
df = df.dropna(subset=["close_btc", "close_eth"])
```

## Streaming recipe (Python)

In a live `Strategy`, subscribe to both symbols, buffer the last
seen value per symbol, and tick the indicator only when both have
a fresh value:

```python
import flox_py as flox

class CorrelatedPair(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)  # [btc_id, eth_id]
        self._btc_id, self._eth_id = symbols
        self._last_btc: float | None = None
        self._last_eth: float | None = None
        self._corr = flox.Correlation(period=30)

    def on_trade(self, ctx, trade):
        if ctx.symbol_id == self._btc_id:
            self._last_btc = trade.price
        elif ctx.symbol_id == self._eth_id:
            self._last_eth = trade.price
        if self._last_btc is None or self._last_eth is None:
            return
        # Tick the indicator at most once per trade event. The
        # contract: the most-recent observation on each symbol
        # represents the value at this instant.
        c = self._corr.update(self._last_btc, self._last_eth)
        if c is not None and abs(c) < 0.2:
            # Pair has decorrelated — example signal.
            self.market_buy(self._btc_id, qty=0.01)
```

This pattern is intentionally manual. The alternative — emitting
a "synced bar" event from the engine — couples the runtime to a
pairing decision (which symbol leads? what timeout?) that varies
per strategy. Keeping it caller-side keeps the engine honest.

## Node.js

```javascript
const flox = require('flox');

const corr = new flox.Correlation(30);
for (let i = 0; i < btcClose.length; ++i) {
  const v = corr.update(btcClose[i], ethClose[i]);
  // v is null until the window fills.
}
```

The streaming pattern in a `Runner` strategy mirrors the Python
example above: cache the last value per symbol in `onTrade`,
tick the indicator when both sides have data.

## Codon

```python
from flox.indicators import Correlation

corr = Correlation(period=30)
for b, e in zip(btc_close, eth_close):
    corr.update(b, e)
```

## What about `AutoCorrelation`?

`AutoCorrelation(window, lag)` is single-input — it correlates a
series with a *lagged version of itself*. To use it across
symbols (cross-correlation at a lag), still align both streams as
above, then subtract / pass the differential through
`AutoCorrelation`. A first-class cross-correlation indicator is
not in the surface today; track on the indicator wishlist.

## See also

- [`multi-symbol-indicators.md`](multi-symbol-indicators.md) —
  the same single-input indicator across many symbols.
- [`multi-feed-clock.md`](multi-feed-clock.md) — the alignment
  primitive for streaming pair-trade decisions.
- [`../explanation/indicators.md#correlation`](../explanation/indicators.md#correlation) —
  what `Correlation` computes and how the streaming variant
  manages its window.
