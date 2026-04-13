# Aggregators

Aggregate raw trades into bars using different policies. All functions accept numpy arrays and release the GIL.

## Common Parameters

All aggregation functions share the same input signature:

| Parameter | Type | Description |
|-----------|------|-------------|
| `timestamps` | `int64[]` | Trade timestamps in nanoseconds |
| `prices` | `float64[]` | Trade prices |
| `quantities` | `float64[]` | Trade quantities |
| `is_buy` | `uint8[]` | Buy/sell flag (1 = buy, 0 = sell) |

**Returns:** `numpy.ndarray` with `PyExtBar` structured dtype.

### PyExtBar Dtype

| Field | Type | Description |
|-------|------|-------------|
| `start_time_ns` | `int64` | Bar open timestamp (unix ns) |
| `end_time_ns` | `int64` | Bar close timestamp (unix ns) |
| `open_raw` | `int64` | Open price * 10^8 |
| `high_raw` | `int64` | High price * 10^8 |
| `low_raw` | `int64` | Low price * 10^8 |
| `close_raw` | `int64` | Close price * 10^8 |
| `volume_raw` | `int64` | Total volume * 10^8 |
| `buy_volume_raw` | `int64` | Buy volume * 10^8 |
| `trade_count` | `int64` | Number of trades in bar |

---

## Functions

### `aggregate_time_bars(..., interval_seconds)`

Aggregate trades into fixed time intervals.

```python
bars = flox.aggregate_time_bars(timestamps, prices, quantities, is_buy,
                                 interval_seconds=60.0)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `interval_seconds` | `float` | Bar duration in seconds |

### `aggregate_tick_bars(..., tick_count)`

Aggregate trades into bars with a fixed number of trades.

```python
bars = flox.aggregate_tick_bars(timestamps, prices, quantities, is_buy,
                                 tick_count=100)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `tick_count` | `int` | Trades per bar |

### `aggregate_volume_bars(..., volume_threshold)`

Aggregate trades into bars when cumulative volume exceeds a threshold.

```python
bars = flox.aggregate_volume_bars(timestamps, prices, quantities, is_buy,
                                   volume_threshold=1000.0)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `volume_threshold` | `float` | Volume per bar |

### `aggregate_range_bars(..., range_size)`

Aggregate trades into bars when price range exceeds a threshold.

```python
bars = flox.aggregate_range_bars(timestamps, prices, quantities, is_buy,
                                  range_size=10.0)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `range_size` | `float` | Maximum price range per bar |

### `aggregate_renko_bars(..., brick_size)`

Aggregate trades into Renko bars with a fixed brick size.

```python
bars = flox.aggregate_renko_bars(timestamps, prices, quantities, is_buy,
                                  brick_size=50.0)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `brick_size` | `float` | Renko brick size |

### `aggregate_heikin_ashi_bars(..., interval_seconds)`

Aggregate trades into Heikin-Ashi bars (smoothed candlesticks).

```python
bars = flox.aggregate_heikin_ashi_bars(timestamps, prices, quantities, is_buy,
                                        interval_seconds=60.0)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `interval_seconds` | `float` | Bar duration in seconds |

---

## Example

```python
import numpy as np
import flox_py as flox

# Load trade data
reader = flox.DataReader("./data")
trades = reader.read_trades()

ts = trades['exchange_ts_ns']
px = trades['price_raw'] / 1e8
qty = trades['qty_raw'] / 1e8
side = trades['side']

# Create 1-minute time bars
bars = flox.aggregate_time_bars(ts, px, qty, side, interval_seconds=60.0)
print(f"Generated {len(bars)} time bars")

# Create 500-tick bars
tick_bars = flox.aggregate_tick_bars(ts, px, qty, side, tick_count=500)
print(f"Generated {len(tick_bars)} tick bars")

# Access bar data
opens = bars['open_raw'] / 1e8
highs = bars['high_raw'] / 1e8
lows = bars['low_raw'] / 1e8
closes = bars['close_raw'] / 1e8
```
