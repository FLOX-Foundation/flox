# Profiles

Order flow and volume analysis tools: footprint bars, volume profile, and market profile (TPO).

## FootprintBar

Tracks bid/ask volume at each price level within a bar for order flow analysis.

```python
fp = flox.FootprintBar(tick_size=0.01)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `tick_size` | `float` | Price level granularity |

### Methods

#### `add_trade(price, quantity, is_buy)`

Add a single trade.

#### `add_trades(prices, quantities, is_buy)`

Add trades from numpy arrays (GIL released).

```python
fp.add_trades(
    prices=np.array([100.0, 100.01, 100.0]),
    quantities=np.array([5.0, 3.0, 2.0]),
    is_buy=np.array([1, 0, 1], dtype=np.uint8),
)
```

#### `total_delta() -> float`

Net delta (buy volume - sell volume).

#### `total_volume() -> float`

Total volume across all levels.

#### `num_levels() -> int`

Number of price levels with activity.

#### `levels() -> list[dict]`

All price levels with volume breakdown.

```python
for lvl in fp.levels():
    print(f"{lvl['price']}: bid={lvl['bid_volume']}, ask={lvl['ask_volume']}, "
          f"delta={lvl['delta']}, imbalance={lvl['imbalance_ratio']:.2f}")
```

| Key | Type | Description |
|-----|------|-------------|
| `price` | `float` | Price level |
| `bid_volume` | `float` | Buy volume at level |
| `ask_volume` | `float` | Sell volume at level |
| `delta` | `float` | Bid - ask volume |
| `imbalance_ratio` | `float` | Volume imbalance ratio |
| `total_volume` | `float` | Total volume at level |

#### `highest_buying_pressure() -> float`

Price with the most buy volume.

#### `highest_selling_pressure() -> float`

Price with the most sell volume.

#### `strongest_imbalance(threshold=0.7) -> float | None`

Price level with the strongest volume imbalance above threshold, or `None`.

#### `clear()`

Reset all data.

---

## VolumeProfile

Volume distribution across price levels. Computes POC, value area, and per-level delta.

```python
vp = flox.VolumeProfile(tick_size=0.01)
```

### Methods

#### `add_trade(price, quantity, is_buy)`

Add a single trade.

#### `add_trades(prices, quantities, is_buy)`

Add trades from numpy arrays (GIL released).

#### `poc() -> float`

Point of Control — price level with the highest volume.

#### `value_area_high() -> float`

Upper bound of the value area (70% of volume).

#### `value_area_low() -> float`

Lower bound of the value area.

#### `total_volume() -> float`

Total volume across all levels.

#### `total_delta() -> float`

Net delta across all levels.

#### `num_levels() -> int`

Number of active price levels.

#### `levels() -> list[dict]`

All levels with volume breakdown.

| Key | Type | Description |
|-----|------|-------------|
| `price` | `float` | Price level |
| `volume` | `float` | Total volume |
| `buy_volume` | `float` | Buy volume |
| `sell_volume` | `float` | Sell volume |
| `delta` | `float` | Buy - sell volume |

#### `volume_at(price) -> float`

Volume at a specific price level.

#### `clear()`

Reset all data.

---

## MarketProfile

Market Profile (TPO) aggregator. Tracks price activity across time periods for auction theory analysis.

```python
mp = flox.MarketProfile(tick_size=0.01, period_minutes=30, session_start_ns=start_ns)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `tick_size` | `float` | Price level granularity |
| `period_minutes` | `int` | TPO period duration |
| `session_start_ns` | `int64` | Session start timestamp (unix ns) |

### Methods

#### `add_trade(timestamp_ns, price, quantity, is_buy)`

Add a single trade with timestamp.

#### `add_trades(timestamps_ns, prices, quantities, is_buy)`

Add trades from numpy arrays (GIL released).

#### `poc() -> float`

Point of Control.

#### `value_area_high() -> float`

Value area high.

#### `value_area_low() -> float`

Value area low.

#### `initial_balance_high() -> float`

Initial balance high (first period's high).

#### `initial_balance_low() -> float`

Initial balance low (first period's low).

#### `single_prints() -> list[float]`

Price levels visited in only one TPO period (potential support/resistance).

#### `is_poor_high() -> bool`

Whether the profile has a poor high (weak auction at top).

#### `is_poor_low() -> bool`

Whether the profile has a poor low.

#### `num_levels() -> int`

Number of price levels.

#### `current_period() -> int`

Current TPO period index.

#### `levels() -> list[dict]`

All levels with TPO data.

| Key | Type | Description |
|-----|------|-------------|
| `price` | `float` | Price level |
| `tpo_count` | `int` | Number of TPO periods at this level |
| `is_single_print` | `bool` | Only visited in one period |

#### `clear()`

Reset all data.
