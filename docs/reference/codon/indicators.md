# Indicators

Technical indicators for Codon strategies. Two types:

1. **Batch** — compute over an entire array at once (calls C++ via C API)
2. **Streaming** — update one value at a time per tick (pure Codon, compiled to native)

---

## Batch indicators

Use these for historical data processing outside the hot path.

```codon
from flox.indicators import ema, sma, rsi, atr, macd, bollinger
```

| Function | Returns | Description |
|----------|---------|-------------|
| `ema(data, period)` | `List[float]` | Exponential Moving Average |
| `sma(data, period)` | `List[float]` | Simple Moving Average |
| `rsi(data, period)` | `List[float]` | Relative Strength Index |
| `atr(high, low, close, period)` | `List[float]` | Average True Range |
| `macd(data, fast=12, slow=26, signal=9)` | `MacdResult` | MACD |
| `bollinger(data, period=20, multiplier=2.0)` | `BollingerResult` | Bollinger Bands |

`MacdResult` fields: `.line`, `.signal`, `.histogram`.

`BollingerResult` fields: `.upper`, `.middle`, `.lower`.

```codon
from flox.indicators import ema, atr, macd

values = ema(prices, 20)

m = macd(prices, 12, 26, 9)
print(m.line[-1], m.signal[-1])

ranges = atr(highs, lows, closes, 14)
```

---

## Streaming indicators

All streaming indicators share the same pattern: call `update()` each tick, read `.value`, check `.ready`.

```codon
from flox.indicators import EMA, SMA, RSI, ATR, MACD, Bollinger
from flox.indicators import RMA, DEMA, TEMA, KAMA, Slope
from flox.indicators import OBV, VWAP, CVD
```

### Single-price indicators

#### `EMA`

```codon
ema = EMA(period=20)
value = ema.update(price)
if ema.ready:
    print(ema.value)
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `__init__(period)` | | |
| `update(value)` | `float` | Feed new value, returns current EMA |
| `value` | `float` | Current value |
| `ready` | `bool` | True after `period` values |

#### `SMA`

Same API as `EMA`. Uses a circular buffer for O(1) updates.

```codon
sma = SMA(period=20)
value = sma.update(price)
```

#### `RMA`

Wilder's Moving Average (used internally by RSI and ATR).

```codon
rma = RMA(period=14)
value = rma.update(price)
```

Same API as `EMA`.

#### `DEMA`

Double Exponential Moving Average.

```codon
dema = DEMA(period=20)
value = dema.update(price)
```

Same API as `EMA`. `.ready` is true after `2 * period` values.

#### `TEMA`

Triple Exponential Moving Average.

```codon
tema = TEMA(period=20)
value = tema.update(price)
```

Same API as `EMA`. `.ready` is true after `3 * period` values.

#### `KAMA`

Kaufman's Adaptive Moving Average.

```codon
kama = KAMA(period=10)
value = kama.update(price)
```

Same API as `EMA`.

#### `Slope`

Linear regression slope over a rolling window.

```codon
slope = Slope(period=20)
value = slope.update(price)
```

Same API as `EMA`.

#### `RSI`

```codon
rsi = RSI(period=14)
value = rsi.update(price)
if rsi.ready:
    print(rsi.value)  # 0..100
```

Same API as `EMA`.

---

### Multi-value indicators

#### `ATR`

```codon
atr = ATR(period=14)
value = atr.update(high, low, close)
if atr.ready:
    print(atr.value)
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `__init__(period)` | | |
| `update(high, low, close)` | `float` | Feed OHLC values |
| `value` | `float` | Current ATR |
| `ready` | `bool` | True after `period` bars |

#### `MACD`

```codon
macd = MACD(fast=12, slow=26, signal=9)
macd.update(price)
if macd.ready:
    print(macd.line, macd.signal, macd.histogram)
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `__init__(fast, slow, signal)` | | |
| `update(value)` | `None` | Feed new value |
| `line` | `float` | MACD line (fast EMA − slow EMA) |
| `signal` | `float` | Signal line (EMA of MACD line) |
| `histogram` | `float` | `line − signal` |
| `ready` | `bool` | True after `slow + signal` values |

#### `Bollinger`

```codon
bb = Bollinger(period=20, multiplier=2.0)
bb.update(price)
if bb.ready:
    print(bb.upper, bb.middle, bb.lower)
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `__init__(period, multiplier=2.0)` | | |
| `update(value)` | `None` | Feed new value |
| `upper` | `float` | Upper band |
| `middle` | `float` | Middle band (SMA) |
| `lower` | `float` | Lower band |
| `ready` | `bool` | True after `period` values |

---

### Volume indicators

#### `OBV`

On-Balance Volume.

```codon
obv = OBV()
value = obv.update(price, volume, is_buy)
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `__init__()` | | |
| `update(price, volume, is_buy)` | `float` | Feed trade data |
| `value` | `float` | Current OBV |

#### `VWAP`

Volume Weighted Average Price.

```codon
vwap = VWAP()
value = vwap.update(price, volume)
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `__init__()` | | |
| `update(price, volume)` | `float` | Feed price and volume |
| `value` | `float` | Current VWAP |
| `reset()` | `None` | Reset accumulator (e.g. session start) |

#### `CVD`

Cumulative Volume Delta.

```codon
cvd = CVD()
value = cvd.update(volume, is_buy)
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `__init__()` | | |
| `update(volume, is_buy)` | `float` | Feed volume |
| `value` | `float` | Current CVD (buy vol − sell vol) |
| `reset()` | `None` | Reset accumulator |

---

## Aliases

`StreamingEMA` and `StreamingSMA` are available as aliases for `EMA` and `SMA` for backward compatibility.
