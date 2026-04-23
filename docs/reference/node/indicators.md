# Indicators

---

## Batch functions

Take `Float64Array` inputs, return `Float64Array` (or object for multi-output).

| Function | Signature | Returns |
|----------|-----------|---------|
| `sma(input, period)` | | `Float64Array` |
| `ema(input, period)` | | `Float64Array` |
| `rsi(input, period)` | | `Float64Array` |
| `rma(input, period)` | Wilder's MA | `Float64Array` |
| `dema(input, period)` | | `Float64Array` |
| `tema(input, period)` | | `Float64Array` |
| `kama(input, period)` | | `Float64Array` |
| `slope(input, length)` | | `Float64Array` |
| `atr(high, low, close, period)` | | `Float64Array` |
| `adx(high, low, close, period)` | | `{ adx, plusDi, minusDi }` |
| `macd(input, fast, slow, signal)` | | `{ line, signal, histogram }` |
| `bollinger(input, period, stdDev)` | | `{ upper, middle, lower }` |
| `stochastic(high, low, close, kPeriod, dPeriod)` | | `{ k, d }` |
| `cci(high, low, close, period)` | | `Float64Array` |
| `chop(high, low, close, period)` | | `Float64Array` |
| `obv(close, volume)` | | `Float64Array` |
| `vwap(close, volume, window)` | | `Float64Array` |
| `cvd(open, high, low, close, volume)` | Cumulative volume delta | `Float64Array` |

---

## Streaming classes

All streaming indicators share the same interface:

```javascript
const ind = new flox.EMA(14);
ind.update(price);   // returns current value
ind.value            // current value (property)
ind.ready            // true once warmed up (property)
```

Multi-output indicators have additional properties instead of `value`:

| Class | Constructor | Extra properties |
|-------|-------------|-----------------|
| `SMA(period)` | | |
| `EMA(period)` | | |
| `RSI(period)` | | |
| `RMA(period)` | Wilder's MA | |
| `DEMA(period)` | | |
| `TEMA(period)` | | |
| `KAMA(period, fast?, slow?)` | | |
| `Slope(length)` | | |
| `ATR(period)` | `update(high, low, close)` | |
| `MACD(fast?, slow?, signal?)` | defaults: 12/26/9 | `line`, `signal`, `histogram` |
| `Bollinger(period, stdDev?)` | default stdDev: 2.0 | `upper`, `middle`, `lower` |
| `Stochastic(kPeriod, dPeriod?)` | default d: 3 | `k`, `d` |
| `CCI(period)` | `update(high, low, close)` | |
| `OBV()` | `update(close, volume)` | |
| `VWAP(window)` | `update(close, volume)` | |
| `CVD()` | `update(open, high, low, close, volume)` | |
