# Python API Reference

Complete reference for the `flox_py` Python module. All classes and functions are available directly from `import flox_py`.

## Installation

```bash
pip install flox-py
```

Or build from source:

```bash
cmake -B build -DFLOX_ENABLE_PYTHON=ON -DFLOX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Modules

| Module | Description |
|--------|-------------|
| [Engine & Backtest](engine.md) | Backtest engine, signal creation, batch execution |
| [Indicators](indicators.md) | 20+ technical indicators (EMA, RSI, MACD, ATR, ...) |
| [Aggregators](aggregators.md) | Bar aggregation (time, tick, volume, range, renko, Heikin-Ashi) |
| [Order Books](books.md) | NLevelOrderBook, L3OrderBook, CompositeBookMatrix |
| [Profiles](profiles.md) | Footprint bars, volume profile, market profile |
| [Positions](positions.md) | Position tracking, group management, order tracking |
| [Replay](replay.md) | Binary log reader/writer, market data recorder |
| [Segment Ops](segment_ops.md) | Merge, split, export, validate, partition data |
| [Backtest Components](backtest.md) | SimulatedExecutor, fills, trade records |
| [Optimizer](optimizer.md) | Permutation test, correlation, bootstrap CI |

## Conventions

- **Prices and quantities** are passed as `float64` and converted to fixed-point (`int64`, scale 10^8) internally.
- **Timestamps** are `int64` nanoseconds. Millisecond and microsecond inputs are auto-detected and converted.
- **Numpy arrays** are the primary data format. Structured arrays use packed C structs for zero-copy access.
- **GIL release**: All compute-heavy functions release the Python GIL for true parallelism.
- **Raw values**: Fields ending in `_raw` are fixed-point integers (value * 10^8). Divide by `1e8` to get floats.
