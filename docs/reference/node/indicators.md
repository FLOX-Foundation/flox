# Indicators

---

## Batch functions

Input arrays are `Float64Array`. Single-output functions return `Float64Array`.

```javascript
const ema  = flox.ema(closes, 20);
const macd = flox.macd(closes, 12, 26, 9);       // { line, signal, histogram }
const bb   = flox.bollinger(closes, 20, 2.0);     // { upper, middle, lower }
const st   = flox.stochastic(hi, lo, cl, 14, 3); // { k, d }
const adx  = flox.adx(hi, lo, cl, 14);           // { adx, plusDi, minusDi }
```

**Single value** — `(input, period)`:

`sma`, `ema`, `rma`, `rsi`, `dema`, `tema`, `kama`, `slope`

**OHLC input:**

`atr(high, low, close, period)`, `cci(high, low, close, period)`, `chop(high, low, close, period)`,
`parkinson_vol(high, low, period)`, `rogers_satchell_vol(open, high, low, close, period)`

**Statistical** — `(input, period)`:

`skewness`, `kurtosis`, `rolling_zscore`, `shannon_entropy(input, period, bins)`, `correlation(x, y, period)`

**Volume:**

`obv(close, volume)`, `vwap(close, volume, window)`, `cvd(open, high, low, close, volume)`

---

## Streaming classes

All streaming indicators share the same interface:

```javascript
const ind = new flox.EMA(14);
ind.update(price);   // returns current value (null during warmup)
ind.value            // current value
ind.ready            // true once warmed up
ind.reset()          // clear state, keep config
```

**Single value** — `update(value)`:

`SMA(period)`, `EMA(period)`, `RMA(period)`, `RSI(period)`, `DEMA(period)`, `TEMA(period)`,
`KAMA(period, fast?, slow?)`, `Slope(length)`, `Skewness(period)`, `Kurtosis(period)`,
`RollingZScore(period)`, `ShannonEntropy(period, bins)`

**Multi-output** — `update(value)`, named properties instead of `.value`:

`MACD(fast?, slow?, signal?)` — defaults 12/26/9 → `.line`, `.signal`, `.histogram`  
`Bollinger(period, stdDev?)` — default stdDev 2.0 → `.upper`, `.middle`, `.lower`

**OHLC / multi-input:**

`ATR(period)` — `update(high, low, close)`  
`Stochastic(kPeriod, dPeriod?)` — `update(high, low, close)` → `.k`, `.d`  
`CCI(period)` — `update(high, low, close)`  
`ParkinsonVol(period)` — `update(high, low)`  
`RogersSatchellVol(period)` — `update(open, high, low, close)`  
`Correlation(period)` — `update(x, y)`

**Volume:**

`OBV()` — `update(close, volume)`  
`VWAP(window)` — `update(close, volume)`  
`CVD()` — `update(open, high, low, close, volume)`

## Indicator catalog

<!-- INDICATOR-LIST-START -->

Every indicator below is **one Node.js class** with both a batch
`compute()` method and streaming `update()` / `value` / `ready` / `reset()`.
Same instance, two ways to use it:

```js
const flox = require('flox-node');
const ema = new flox.EMA(10);
const out = ema.compute(prices);            // batch
for (const v of stream) {
  ema.update(v);
  if (ema.ready) console.log(ema.value);    // streaming on the same instance
}
```

| Indicator | Constructor | Kind |
|---|---|---|
| `EMA` | `new flox.EMA(period)` | SingleInput |
| `SMA` | `new flox.SMA(period)` | SingleInput |
| `RMA` | `new flox.RMA(period)` | SingleInput |
| `RSI` | `new flox.RSI(period)` | SingleInput |
| `KAMA` | `new flox.KAMA(period, fast, slow)` | SingleInput |
| `DEMA` | `new flox.DEMA(period)` | SingleInput |
| `TEMA` | `new flox.TEMA(period)` | SingleInput |
| `Slope` | `new flox.Slope(length)` | SingleInput |
| `Skewness` | `new flox.Skewness(period)` | SingleInput |
| `Kurtosis` | `new flox.Kurtosis(period)` | SingleInput |
| `RollingZScore` | `new flox.RollingZScore(period)` | SingleInput |
| `ShannonEntropy` | `new flox.ShannonEntropy(period, bins)` | SingleInput |
| `AutoCorrelation` | `new flox.AutoCorrelation(window, lag)` | SingleInput |
| `ATR` | `new flox.ATR(period)` | BarInput |
| `CCI` | `new flox.CCI(period)` | BarInput |
| `Stochastic` | `new flox.Stochastic(k_period, d_period)` | BarInput |
| `ParkinsonVol` | `new flox.ParkinsonVol(period)` | HighLowInput |
| `RogersSatchellVol` | `new flox.RogersSatchellVol(period)` | OhlcInput |
| `Correlation` | `new flox.Correlation(period)` | PairInput |
| `MACD` | `new flox.MACD(fast, slow, signal)` | MultiOutput |
| `Bollinger` | `new flox.Bollinger(period, stddev)` | MultiOutput |


<!-- INDICATOR-LIST-END -->
