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
