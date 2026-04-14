# flox-py

Python bindings for the [FLOX](https://github.com/FLOX-Foundation/flox) trading framework.

## Install

```bash
pip install flox-py
```

## Quick Start

```python
import flox_py as flox
import numpy as np

# Indicators
ema = flox.ema(close, 50)
rsi = flox.rsi(close, 14)
atr = flox.atr(high, low, close, 14)

# Backtest engine
engine = flox.Engine(initial_capital=100_000, fee_rate=0.0001)
engine.load_bars_df(timestamps, opens, highs, lows, closes, volumes)
signals = flox.make_signals(timestamps, sides, quantities)
stats = engine.run(signals)

# 1000 backtests in parallel
results = engine.run_batch([signals_1, signals_2, ...])
```

## Modules

| Module | Description |
|--------|-------------|
| Engine | Backtest engine with batch execution |
| Indicators | EMA, SMA, RSI, MACD, ATR, Bollinger, ADX, Stochastic, CCI, VWAP, CVD, and more |
| Aggregators | Time, tick, volume, range, renko, Heikin-Ashi bars |
| Order Books | N-level, L3, cross-exchange CompositeBookMatrix |
| Profiles | Footprint bars, volume profile, market profile |
| Positions | Position tracking with FIFO/LIFO/average cost basis |
| Replay | Binary log reader/writer, market data recorder |
| Segment Ops | Merge, split, export, validate, partition data |
| Optimizer | Permutation test, bootstrap CI, correlation |

All compute-heavy operations release the GIL for true parallelism.

Full API reference at [flox-foundation.github.io/flox/reference/python](https://flox-foundation.github.io/flox/reference/python/).
