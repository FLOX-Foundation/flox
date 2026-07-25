# Indicators

Technical indicators for Codon strategies. Two types:

1. **Batch** — compute over an entire array at once (calls C++ via C API)
2. **Streaming** — update one value at a time per tick (pure Codon, compiled to native)

---

## Batch indicators

Batch indicators are **free functions**, not class methods. Import them by name.

```codon
from flox.indicators import ema, sma, rsi, atr, macd, bollinger
from flox.indicators import skewness, kurtosis, rolling_zscore, shannon_entropy
from flox.indicators import parkinson_vol, rogers_satchell_vol, correlation, autocorrelation
from flox.indicators import obv, vwap, cvd, chop, adx, adf, stochastic
```

**Single value** — returns `List[float]`:

`ema(data, period)`, `sma(data, period)`, `rma(data, period)`, `rsi(data, period)`, `dema(data, period)`, `tema(data, period)`, `kama(data, period, fast=2, slow=30)`, `slope(data, length)`

**OHLC / multi-input** — returns `List[float]`:

`atr(high, low, close, period)`, `cci(high, low, close, period)`, `chop(high, low, close, period)`, `parkinson_vol(high, low, period)`, `rogers_satchell_vol(open_, high, low, close, period)`, `correlation(x, y, period)`

**Statistical** — returns `List[float]`:

`skewness(data, period)`, `kurtosis(data, period)`, `rolling_zscore(data, period)`, `shannon_entropy(data, period, bins=10)`, `autocorrelation(data, window, lag)`

**Volume** — returns `List[float]`:

`obv(close, volume)`, `vwap(close, volume, window)`, `cvd(open_, high, low, close, volume)`

**Multi-output:**

`macd(data, fast=12, slow=26, signal=9)` — returns `MacdResult`: `.line`, `.signal`, `.histogram`
`bollinger(data, period=20, multiplier=2.0)` — returns `BollingerResult`: `.upper`, `.middle`, `.lower`
`stochastic(high, low, close, k_period=14, d_period=3)` — returns `StochasticResult`: `.k`, `.d`
`adx(high, low, close, period=14)` — returns `AdxResult`: `.adx`, `.plus_di`, `.minus_di`
`adf(data, max_lag=4, regression="c")` — returns `AdfResult`: `.test_stat`, `.p_value`, `.used_lag` (scalars, not lists)

```codon
values = ema(prices, 20)

m = macd(prices, 12, 26, 9)
print(m.line[-1], m.signal[-1])

ranges = atr(highs, lows, closes, 14)

skew = skewness(prices, 20)
```

---

## Streaming indicators

All streaming indicators share the same pattern: call `update()` each tick, read `.value`, check `.ready`. All support `.reset()` to clear state.

```codon
from flox.indicators import EMA, SMA, RSI, ATR, MACD, Bollinger
from flox.indicators import RMA, DEMA, TEMA, KAMA, Slope, CCI, Stochastic
from flox.indicators import Skewness, Kurtosis, RollingZScore, ShannonEntropy
from flox.indicators import ParkinsonVol, RogersSatchellVol, Correlation, AutoCorrelation
```

The complete class list is `SMA`, `EMA`, `RMA`, `DEMA`, `TEMA`, `KAMA`, `RSI`, `Slope`, `ATR`, `CCI`,
`MACD`, `Bollinger`, `Stochastic`, `Skewness`, `Kurtosis`, `RollingZScore`, `ShannonEntropy`,
`ParkinsonVol`, `RogersSatchellVol`, `Correlation`, `AutoCorrelation`. There is no `OBV`, `VWAP` or
`CVD` class — those three are batch-only free functions.

Streaming classes accumulate history internally and re-run the batch function over it, so `.value`
is by construction identical to the last element of the batch result.

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

#### `CCI`

Commodity Channel Index.

```codon
cci = CCI(period=20)
value = cci.update(high, low, close)
```

#### `Stochastic`

```codon
st = Stochastic(k_period=14, d_period=3)
st.update(high, low, close)
if st.ready:
    print(st.k, st.d)
```

#### `AutoCorrelation`

Rolling autocorrelation at a fixed lag. `.ready` is true after `window + lag` values.

```codon
ac = AutoCorrelation(window=50, lag=1)
value = ac.update(price)
```

---

### Volume indicators

There are no streaming volume-indicator classes. On-Balance Volume, VWAP and Cumulative Volume Delta
are batch-only free functions:

```codon
from flox.indicators import obv, vwap, cvd

obv_series  = obv(closes, volumes)
vwap_series = vwap(closes, volumes, 20)
cvd_series  = cvd(opens, highs, lows, closes, volumes)
```

---

## Batch vs streaming, per class

Not every class carries a batch `compute()`. Classes **with** an instance `compute()`: `SMA`, `EMA`,
`RMA`, `DEMA`, `TEMA`, `KAMA`, `RSI`, `Slope`, `ATR`, `MACD`, `Bollinger`. `SMA` additionally has a
`compute_static(data, period)` — it is the only static form in the module.

Classes with **no** `compute()` — streaming only: `CCI`, `Stochastic`, `Skewness`, `Kurtosis`,
`RollingZScore`, `ShannonEntropy`, `ParkinsonVol`, `RogersSatchellVol`, `Correlation`,
`AutoCorrelation`. For batch use, call the corresponding free function.

!!! warning "The generated catalog below overstates this"
    The auto-generated table is produced from `include/flox/indicator/registry.def`, which describes
    the C++ indicator surface. Its claim that every indicator is one class with both `compute()` and
    `update()` does not hold for the Codon module, and the constructor column renders C++ signatures
    (`flox.EMA(size_t period)`). Read the table as a list of available class names only; use this
    page's sections above for the Codon signatures.

## Indicator catalog

<!-- INDICATOR-LIST-START -->

Every indicator below is **one Codon class** with both a batch
`compute()` method and streaming `update()` / `value` / `ready` / `reset()`.

| Indicator | Constructor | Kind |
|---|---|---|
| `EMA` | `flox.EMA(size_t period)` | SingleInput |
| `SMA` | `flox.SMA(size_t period)` | SingleInput |
| `RMA` | `flox.RMA(size_t period)` | SingleInput |
| `RSI` | `flox.RSI(size_t period)` | SingleInput |
| `KAMA` | `flox.KAMA(size_t period, size_t fast, size_t slow)` | SingleInput |
| `DEMA` | `flox.DEMA(size_t period)` | SingleInput |
| `TEMA` | `flox.TEMA(size_t period)` | SingleInput |
| `Slope` | `flox.Slope(size_t length)` | SingleInput |
| `Skewness` | `flox.Skewness(size_t period)` | SingleInput |
| `Kurtosis` | `flox.Kurtosis(size_t period)` | SingleInput |
| `RollingZScore` | `flox.RollingZScore(size_t period)` | SingleInput |
| `ShannonEntropy` | `flox.ShannonEntropy(size_t period, size_t bins)` | SingleInput |
| `AutoCorrelation` | `flox.AutoCorrelation(size_t window, size_t lag)` | SingleInput |
| `ATR` | `flox.ATR(size_t period)` | BarInput |
| `CCI` | `flox.CCI(size_t period)` | BarInput |
| `Stochastic` | `flox.Stochastic(size_t k_period, size_t d_period)` | BarInput |
| `ParkinsonVol` | `flox.ParkinsonVol(size_t period)` | HighLowInput |
| `RogersSatchellVol` | `flox.RogersSatchellVol(size_t period)` | OhlcInput |
| `Correlation` | `flox.Correlation(size_t period)` | PairInput |
| `MACD` | `flox.MACD(size_t fast, size_t slow, size_t signal)` | MultiOutput |
| `Bollinger` | `flox.Bollinger(size_t period, double stddev)` | MultiOutput |


<!-- INDICATOR-LIST-END -->
