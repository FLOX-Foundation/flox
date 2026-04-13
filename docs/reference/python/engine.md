# Engine & Backtest

The core backtest engine. Load OHLCV data once, then run unlimited backtests with different signal sets.

## Engine

```python
engine = flox.Engine(initial_capital=100_000, fee_rate=0.0001)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `initial_capital` | `float` | `100000.0` | Starting capital |
| `fee_rate` | `float` | `0.0001` | Fee rate per trade (percentage mode) |

### Methods

#### `load_bars(bars)`

Load bars from a numpy structured array with `PyBar` dtype.

```python
bars = np.zeros(n, dtype=flox.PyBar)
bars['timestamp_ns'] = timestamps
bars['open_raw'] = (opens * 1e8).astype(np.int64)
bars['high_raw'] = (highs * 1e8).astype(np.int64)
bars['low_raw'] = (lows * 1e8).astype(np.int64)
bars['close_raw'] = (closes * 1e8).astype(np.int64)
bars['volume_raw'] = (volumes * 1e8).astype(np.int64)
engine.load_bars(bars)
```

#### `load_bars_df(timestamps, open, high, low, close, volume)`

Load bars from separate numpy arrays (float64). Timestamps are auto-normalized to nanoseconds.

```python
engine.load_bars_df(timestamps, opens, highs, lows, closes, volumes)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `timestamps` | `int64[]` | Unix timestamps (s, ms, us, or ns — auto-detected) |
| `open` | `float64[]` | Open prices |
| `high` | `float64[]` | High prices |
| `low` | `float64[]` | Low prices |
| `close` | `float64[]` | Close prices |
| `volume` | `float64[]` | Volume |

#### `run(signals, symbol=1) -> dict`

Run a single backtest. Returns a stats dictionary.

```python
stats = engine.run(signals, symbol=1)
print(stats['net_pnl'], stats['sharpe'])
```

**Return keys:**

| Key | Type | Description |
|-----|------|-------------|
| `total_trades` | `int` | Round-trip trade count |
| `winning_trades` | `int` | Profitable trade count |
| `losing_trades` | `int` | Losing trade count |
| `initial_capital` | `float` | Starting capital |
| `final_capital` | `float` | Ending capital |
| `total_pnl` | `float` | Gross PnL |
| `total_fees` | `float` | Total fees paid |
| `net_pnl` | `float` | PnL after fees |
| `gross_profit` | `float` | Sum of winning trades |
| `gross_loss` | `float` | Sum of losing trades |
| `max_drawdown` | `float` | Maximum drawdown (absolute) |
| `max_drawdown_pct` | `float` | Maximum drawdown (percentage) |
| `win_rate` | `float` | Fraction of winning trades |
| `profit_factor` | `float` | Gross profit / gross loss |
| `avg_win` | `float` | Average winning trade |
| `avg_loss` | `float` | Average losing trade |
| `sharpe` | `float` | Annualized Sharpe ratio |
| `sortino` | `float` | Annualized Sortino ratio |
| `calmar` | `float` | Calmar ratio |
| `return_pct` | `float` | Net return percentage |

#### `run_batch(signal_sets, threads=0, symbol=1) -> list[dict]`

Run N backtests in parallel using C++ threads. GIL released.

```python
results = engine.run_batch([signals_1, signals_2, ...], threads=0)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `signal_sets` | `list` | — | List of signal arrays |
| `threads` | `int` | `0` | Thread count (0 = all cores) |
| `symbol` | `int` | `1` | Symbol ID |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `bar_count` | `int` | Number of loaded bars |

---

## make_signals()

Create a packed signal array from separate numpy arrays.

```python
signals = flox.make_signals(
    timestamps,   # int64 — unix ms, us, or ns
    sides,        # uint8 — 0=buy, 1=sell
    quantities,   # float64 — position size
    prices=None,  # float64 — limit price (optional)
    types=None,   # uint8 — 0=market, 1=limit (optional)
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `timestamps` | `int64[]` | — | Trade timestamps (auto-normalized to ns) |
| `sides` | `uint8[]` | — | 0 = buy, 1 = sell |
| `quantities` | `float64[]` | — | Position sizes |
| `prices` | `float64[]` | `None` | Limit prices (omit for market orders) |
| `types` | `uint8[]` | `None` | Order types: 0 = market, 1 = limit |

**Returns:** `numpy.ndarray` with `PySignal` dtype.

---

## Structured Dtypes

### PySignal

| Field | Type | Description |
|-------|------|-------------|
| `timestamp_ns` | `int64` | Timestamp in nanoseconds |
| `quantity_raw` | `int64` | Quantity * 10^8 |
| `price_raw` | `int64` | Price * 10^8 (0 for market) |
| `side` | `uint8` | 0 = buy, 1 = sell |
| `order_type` | `uint8` | 0 = market, 1 = limit |

### PyBar

| Field | Type | Description |
|-------|------|-------------|
| `timestamp_ns` | `int64` | Bar timestamp in nanoseconds |
| `open_raw` | `int64` | Open * 10^8 |
| `high_raw` | `int64` | High * 10^8 |
| `low_raw` | `int64` | Low * 10^8 |
| `close_raw` | `int64` | Close * 10^8 |
| `volume_raw` | `int64` | Volume * 10^8 |

---

## Example

```python
import numpy as np
import flox_py as flox

engine = flox.Engine(initial_capital=100_000, fee_rate=0.0001)
engine.load_bars_df(timestamps, opens, highs, lows, closes, volumes)

# Simple MA crossover signals
fast = flox.ema(closes, 10)
slow = flox.ema(closes, 30)

cross_up = (fast[1:] > slow[1:]) & (fast[:-1] <= slow[:-1])
cross_down = (fast[1:] < slow[1:]) & (fast[:-1] >= slow[:-1])

ts_list, side_list, qty_list = [], [], []
for i in range(len(cross_up)):
    if cross_up[i]:
        ts_list.append(timestamps[i + 1])
        side_list.append(0)  # buy
        qty_list.append(1.0)
    elif cross_down[i]:
        ts_list.append(timestamps[i + 1])
        side_list.append(1)  # sell
        qty_list.append(1.0)

signals = flox.make_signals(
    np.array(ts_list, dtype=np.int64),
    np.array(side_list, dtype=np.uint8),
    np.array(qty_list, dtype=np.float64),
)

stats = engine.run(signals)
print(f"Net PnL: {stats['net_pnl']:.2f}, Sharpe: {stats['sharpe']:.4f}")
```
