# Indicators

Technical indicators for Codon strategies. Two types:

1. **Batch** — compute over an entire array at once (calls C++ via C API)
2. **Streaming** — update one value at a time per tick (pure Codon, compiled to native)

---

## Batch indicators

```codon
from flox.indicators import ema, sma, rsi, atr, macd, bollinger
from flox.indicators import Skewness, Kurtosis, RollingZScore, ShannonEntropy
from flox.indicators import ParkinsonVol, RogersSatchellVol, Correlation
```

**Single value** — returns `List[float]`:

`ema(data, period)`, `sma(data, period)`, `rma(data, period)`, `rsi(data, period)`, `dema(data, period)`, `tema(data, period)`, `kama(data, period)`

**OHLC / multi-input** — returns `List[float]`:

`atr(high, low, close, period)`, `ParkinsonVol.compute(high, low, period)`, `RogersSatchellVol.compute(open, high, low, close, period)`, `Correlation.compute(x, y, period)`

**Statistical** — returns `List[float]`:

`Skewness.compute(data, period)`, `Kurtosis.compute(data, period)`, `RollingZScore.compute(data, period)`, `ShannonEntropy.compute(data, period, bins)`

**Multi-output:**

`macd(data, fast=12, slow=26, signal=9)` — returns `MacdResult`: `.line`, `.signal`, `.histogram`  
`bollinger(data, period=20, multiplier=2.0)` — returns `BollingerResult`: `.upper`, `.middle`, `.lower`

```codon
values = ema(prices, 20)

m = macd(prices, 12, 26, 9)
print(m.line[-1], m.signal[-1])

ranges = atr(highs, lows, closes, 14)
```

---

## Streaming indicators

All streaming indicators share the same pattern: call `update()` each tick, read `.value`, check `.ready`. All support `.reset()` to clear state.

```codon
from flox.indicators import EMA, SMA, RSI, ATR, MACD, Bollinger
from flox.indicators import RMA, DEMA, TEMA, KAMA, Slope
from flox.indicators import OBV, VWAP, CVD
from flox.indicators import Skewness, Kurtosis, RollingZScore, ShannonEntropy
from flox.indicators import ParkinsonVol, RogersSatchellVol, Correlation
```

### Single-price indicators

#### `EMA`

```codon
ema = EMA(period=20)
value = ema.update(price)
if ema.ready:
    print(ema.value)
```

#### `SMA`

Uses a circular buffer for O(1) updates.

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

#### `DEMA`

Double Exponential Moving Average. `.ready` is true after `2 * period` values.

```codon
dema = DEMA(period=20)
value = dema.update(price)
```

#### `TEMA`

Triple Exponential Moving Average. `.ready` is true after `3 * period` values.

```codon
tema = TEMA(period=20)
value = tema.update(price)
```

#### `KAMA`

Kaufman's Adaptive Moving Average.

```codon
kama = KAMA(period=10)
value = kama.update(price)
```

#### `Slope`

Linear regression slope over a rolling window.

```codon
slope = Slope(period=20)
value = slope.update(price)
```

#### `RSI`

```codon
rsi = RSI(period=14)
value = rsi.update(price)
if rsi.ready:
    print(rsi.value)  # 0..100
```

#### `Skewness`

Fisher-Pearson skewness. Requires period >= 3.

```codon
skew = Skewness(period=20)
value = skew.update(price)
```

#### `Kurtosis`

Fisher excess kurtosis. Requires period >= 4.

```codon
kurt = Kurtosis(period=20)
value = kurt.update(price)
```

#### `RollingZScore`

`(x - mean) / std`.

```codon
zscore = RollingZScore(period=20)
value = zscore.update(price)
```

#### `ShannonEntropy`

Rolling Shannon entropy, normalized to [0, 1].

```codon
ent = ShannonEntropy(period=20, bins=10)
value = ent.update(price)
```

---

### Multi-value indicators

#### `ATR`

```codon
atr = ATR(period=14)
value = atr.update(high, low, close)
```

#### `MACD`

```codon
macd = MACD(fast=12, slow=26, signal=9)
macd.update(price)
if macd.ready:
    print(macd.line, macd.signal, macd.histogram)
```

#### `Bollinger`

```codon
bb = Bollinger(period=20, multiplier=2.0)
bb.update(price)
if bb.ready:
    print(bb.upper, bb.middle, bb.lower)
```

#### `ParkinsonVol`

Parkinson high-low volatility estimator.

```codon
pvol = ParkinsonVol(period=20)
value = pvol.update(high, low)
```

#### `RogersSatchellVol`

Rogers-Satchell OHLC volatility estimator.

```codon
rsv = RogersSatchellVol(period=20)
value = rsv.update(open_, high, low, close)
```

#### `Correlation`

Rolling Pearson correlation between two series.

```codon
corr = Correlation(period=20)
value = corr.update(x, y)
```

---

### Volume indicators

#### `OBV`

On-Balance Volume.

```codon
obv = OBV()
value = obv.update(price, volume, is_buy)
```

#### `VWAP`

Volume Weighted Average Price.

```codon
vwap = VWAP()
value = vwap.update(price, volume)
```

#### `CVD`

Cumulative Volume Delta.

```codon
cvd = CVD()
value = cvd.update(volume, is_buy)
```

---

## Aliases

`StreamingEMA` and `StreamingSMA` are available as aliases for `EMA` and `SMA` for backward compatibility.
