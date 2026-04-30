# Indicators

Each indicator has `.update()` for per-tick use and a static `.compute()` for batch. All classes support `.reset()` to clear state.

```javascript
// Single value
const ema = new EMA(20);
ema.update(price);        // returns current value (NaN during warmup)
ema.reset();

// Multi-output
const macd = new MACD(12, 26, 9);
macd.update(price);
macd.line; macd.signal; macd.histogram;

// OHLC input
const atr = new ATR(14);
atr.update(high, low, close);

const stoch = new Stochastic(14, 3);
stoch.update(high, low, close);
stoch.k; stoch.d;

// Multi-input
const corr = new Correlation(20);
corr.update(x, y);

const pvol = new ParkinsonVol(20);
pvol.update(high, low);

// Batch
const adxResult = ADX.compute(highs, lows, closes, 14);
adxResult.adx; adxResult.plusDi; adxResult.minusDi;

const skewArr = Skewness.compute(prices, 20);
```

**Single value** — `update(value)`:

`SMA`, `EMA`, `RMA`, `DEMA`, `TEMA`, `KAMA`, `RSI`, `Slope`, `Skewness`, `Kurtosis`, `RollingZScore`, `ShannonEntropy`

**Multi-output** — named properties instead of `.value`:

`MACD` → `.line`, `.signal`, `.histogram`  
`Bollinger` → `.upper`, `.middle`, `.lower`

**OHLC / multi-input:**

`ATR`, `CCI`, `CHOP` — `update(high, low, close)`  
`Stochastic` — `update(high, low, close)` → `.k`, `.d`  
`ADX` — batch only: `ADX.compute(highs, lows, closes, period)` → `.adx`, `.plusDi`, `.minusDi`  
`ParkinsonVol` — `update(high, low)`  
`RogersSatchellVol` — `update(open, high, low, close)`  
`Correlation` — `update(x, y)`

**Volume:**

`OBV`, `VWAP`, `CVD`

## Indicator catalog

<!-- INDICATOR-LIST-START -->

Every indicator below is **one QuickJS class** with both a batch
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
