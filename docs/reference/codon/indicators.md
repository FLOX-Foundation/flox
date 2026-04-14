# Indicators

Technical indicators available in Codon strategies. Two types are provided:

1. **Batch indicators** -- compute over entire arrays via C API (same as C++ implementations)
2. **Streaming indicators** -- pure Codon, update incrementally per tick (no FFI overhead)

## Batch Indicators

These call the C++ indicator implementations via the C API. Use them for
historical data processing.

### `ema(data, period) -> List[float]`

Exponential Moving Average.

```python
from flox.indicators import ema
result = ema(prices, 20)
```

### `sma(data, period) -> List[float]`

Simple Moving Average.

### `rsi(data, period) -> List[float]`

Relative Strength Index.

### `atr(high, low, close, period) -> List[float]`

Average True Range.

```python
from flox.indicators import atr
result = atr(highs, lows, closes, 14)
```

### `macd(data, fast=12, slow=26, signal=9) -> MacdResult`

MACD indicator. Returns `MacdResult` with `.line`, `.signal`, `.histogram` fields.

```python
from flox.indicators import macd
result = macd(prices, 12, 26, 9)
print(result.line[-1], result.signal[-1])
```

### `bollinger(data, period=20, multiplier=2.0) -> BollingerResult`

Bollinger Bands. Returns `BollingerResult` with `.upper`, `.middle`, `.lower` fields.

```python
from flox.indicators import bollinger
result = bollinger(prices, 20, 2.0)
```

## Streaming Indicators

Pure Codon implementations compiled to native code. Use them in `on_trade()`
callbacks for per-tick incremental computation.

### `StreamingEMA`

```python
from flox.indicators import StreamingEMA

ema = StreamingEMA(period=20)

# In on_trade callback:
value = ema.update(price)
if ema.ready:
    print(f"EMA: {value}")
```

#### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `__init__(period)` | | Create with given period |
| `update(value)` | `float` | Feed new value, returns current EMA |
| `value` | `float` | Current EMA value (property) |
| `ready` | `bool` | True after `period` values received |

### `StreamingSMA`

Uses a circular buffer for O(1) update.

```python
from flox.indicators import StreamingSMA

sma = StreamingSMA(period=20)
value = sma.update(price)
```

#### Methods

Same API as `StreamingEMA`.
