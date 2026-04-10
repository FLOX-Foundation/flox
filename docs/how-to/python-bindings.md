# Python Bindings

Run Flox backtests from Python. Strategies stay in Python (numpy/pandas), execution runs in C++ with GIL released and multi-threaded batch support.

## Build

```bash
cmake -B build \
  -DFLOX_ENABLE_BACKTEST=ON \
  -DFLOX_ENABLE_PYTHON=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Requires: Python 3.9+, pybind11, numpy (`pip install pybind11 numpy`).

The module builds at `build/python/flox_py.cpython-*.so`.

## Quick Start

```python
import numpy as np
import flox_py as flox

engine = flox.Engine(initial_capital=100_000, fee_rate=0.0001)

# Load OHLCV data
engine.load_bars_df(timestamps, opens, highs, lows, closes, volumes)

# Create signals (all market orders)
signals = flox.make_signals(
    timestamps=np.array([1704067200000, 1704068400000], dtype=np.int64),
    sides=np.array([0, 1], dtype=np.uint8),  # 0=buy, 1=sell
    quantities=np.array([0.5, 0.5]),
)

stats = engine.run(signals)
print(f"PnL: {stats['net_pnl']:.2f}, Sharpe: {stats['sharpe']:.4f}")
```

## Loading Bar Data

Two options:

=== "From numpy arrays"

    ```python
    engine.load_bars_df(
        timestamps,  # int64, unix ms or ns (auto-detected)
        opens,       # float64
        highs,       # float64
        lows,        # float64
        closes,      # float64
        volumes,     # float64
    )
    ```

=== "From structured array"

    ```python
    bars = np.zeros(n, dtype=flox.PyBar)
    bars['timestamp_ns'] = ...
    bars['open_raw'] = (opens * 1e8).astype(np.int64)
    # ... etc
    engine.load_bars(bars)
    ```

Load once, then run as many backtests as needed against the same data.

## Creating Signals

`make_signals()` converts numpy arrays into a packed struct array:

```python
signals = flox.make_signals(
    timestamps,   # int64 — unix ms, us, or ns (auto-normalized)
    sides,        # uint8 — 0=buy, 1=sell
    quantities,   # float64 — position size
    prices,       # float64 — limit price (optional, default: market)
    types,        # uint8 — 0=market, 1=limit (optional, default: market)
)
```

For market-only strategies, `prices` and `types` can be omitted:

```python
signals = flox.make_signals(timestamps, sides, quantities)
```

## Single Run

```python
stats = engine.run(signals, symbol=1)
```

Returns a dict with all backtest metrics:

| Key | Description |
|-----|-------------|
| `total_trades` | Round-trip trade count |
| `net_pnl` | Gross PnL minus all fees |
| `total_fees` | Total execution fees |
| `sharpe` | Annualized Sharpe ratio |
| `sortino` | Annualized Sortino ratio |
| `calmar` | Calmar ratio |
| `max_drawdown` | Peak-to-trough drawdown |
| `max_drawdown_pct` | Drawdown as percentage |
| `win_rate` | Winning trade fraction |
| `profit_factor` | Gross profit / gross loss |
| `return_pct` | Net return percentage |

## Batch Execution

Run N backtests in parallel using C++ threads:

```python
all_stats = engine.run_batch(
    [signals_1, signals_2, ..., signals_n],
    threads=0,   # 0 = use all cores
    symbol=1,
)
```

GIL released. Threads run independent copies, nothing shared.

### Permutation Testing Example

```python
import flox_py as flox
import numpy as np

engine = flox.Engine(initial_capital=100_000, fee_rate=0.0001)
engine.load_bars_df(timestamps, opens, highs, lows, closes, volumes)

rng = np.random.default_rng(42)
signal_sets = []

for _ in range(1000):
    # Shuffle returns
    rets = np.diff(np.log(closes))
    rng.shuffle(rets)
    shuffled = closes[0] * np.exp(np.cumsum(np.concatenate(([0], rets))))

    # Your strategy logic here
    sigs = my_strategy(shuffled, timestamps)
    signal_sets.append(sigs)

# 1000 backtests in ~50ms
results = engine.run_batch(signal_sets)
pnls = [r["net_pnl"] for r in results]
p_value = np.mean([p >= results[0]["net_pnl"] for p in pnls])
```

## Performance

Benchmarked on Apple M-series, 100K bars, MA cross strategy:

| Mode | Time | vs Python |
|------|------|-----------|
| Single run | 0.6ms | 33x |
| 1000 permutations | 53ms | 400x |
| Bar loading | 0.5ms | — |

## See Also

- [Backtesting](backtest.md) — C++ backtest guide
- [Grid Search](grid-search.md) — parameter optimization
