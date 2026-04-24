# Indicators

Each indicator has `.update()` for per-tick use and a static `.compute()` for batch.

| Single-value | OHLC | Volume |
|---|---|---|
| SMA, EMA, RMA, DEMA, TEMA, KAMA | ATR, ADX, Stochastic, CCI, CHOP | OBV, VWAP, CVD |
| RSI, Slope, MACD, Bollinger | | |

```javascript
// Per-tick
const sma = new SMA(20);
sma.update(price);          // returns current value

const macd = new MACD(12, 26, 9);
macd.update(price);
macd.line; macd.signal; macd.histogram;

const atr = new ATR(14);
atr.update(high, low, close);

// Batch
const result = ADX.compute(highs, lows, closes, 14);
result.adx; result.plusDi; result.minusDi;
```
