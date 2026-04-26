# Indicators

Vectorized technical indicators operating on numpy arrays. All functions release the GIL for parallel computation.

## Streaming classes

All streaming indicators share the same interface:

```python
ind = flox.EMA(20)
result = ind.update(price)  # None during warmup, float when ready
ind.value                   # None or float
ind.ready                   # bool
ind.reset()                 # clear state, keep config
ind2 = ind.fresh()          # new independent instance, same config
```

During warmup, `update()` and `.value` return `None`. Check with `if result is not None` or use `.ready`.

**Single value** — `update(value) -> float | None`:

`SMA(period)`, `EMA(period)`, `RMA(period)`, `RSI(period)`, `DEMA(period)`, `TEMA(period)`, `KAMA(period, fast=2, slow=30)`, `Slope(length)`, `Skewness(period)`, `Kurtosis(period)`, `RollingZScore(period)`, `ShannonEntropy(period, bins=10)`

**Multi-output** — `update(value) -> float | None`, named properties instead of `.value`:

`MACD(fast=12, slow=26, signal=9)` → `.line`, `.signal`, `.histogram`  
`Bollinger(period=20, multiplier=2.0)` → `.upper`, `.middle`, `.lower`

**OHLC / multi-input:**

`ATR(period)` — `update(high, low, close)`  
`Stochastic(k_period=14, d_period=3)` — `update(high, low, close)` → `.k`, `.d`  
`CCI(period=20)` — `update(high, low, close)`  
`ParkinsonVol(period)` — `update(high, low)`  
`RogersSatchellVol(period)` — `update(open, high, low, close)`  
`Correlation(period)` — `update(x, y)`

**Volume:**

`OBV()` — `update(close, volume)`  
`VWAP(window)` — `update(close, volume)`  
`CVD()` — `update(open, high, low, close, volume)`

---

## Batch functions

## Moving Averages

### `ema(input, period) -> ndarray`

Exponential Moving Average.

```python
result = flox.ema(closes, period=20)
```

### `sma(input, period) -> ndarray`

Simple Moving Average.

```python
result = flox.sma(closes, period=20)
```

### `rma(input, period) -> ndarray`

Wilder's Moving Average (alpha = 1/period). Used internally by RSI and ATR.

```python
result = flox.rma(closes, period=14)
```

### `dema(input, period) -> ndarray`

Double EMA: `2*EMA - EMA(EMA)`. Reduces lag versus standard EMA.

```python
result = flox.dema(closes, period=20)
```

### `tema(input, period) -> ndarray`

Triple EMA: `3*EMA - 3*EMA(EMA) + EMA(EMA(EMA))`. Further lag reduction.

```python
result = flox.tema(closes, period=20)
```

### `kama(input, period=10) -> ndarray`

Kaufman Adaptive Moving Average. Adjusts smoothing based on market efficiency.

```python
result = flox.kama(closes, period=10)
```

---

## Oscillators

### `rsi(input, period) -> ndarray`

Relative Strength Index (0–100).

```python
result = flox.rsi(closes, period=14)
```

### `macd(input, fast=12, slow=26, signal=9) -> dict`

Moving Average Convergence Divergence. Returns a dict with three arrays.

```python
result = flox.macd(closes, fast=12, slow=26, signal=9)
# result['line'], result['signal'], result['histogram']
```

### `stochastic(high, low, close, k_period=14, d_period=3) -> dict`

Stochastic oscillator (%K and %D).

```python
result = flox.stochastic(highs, lows, closes, k_period=14, d_period=3)
k = result['k']
d = result['d']
```

### `cci(high, low, close, period=20) -> ndarray`

Commodity Channel Index.

```python
result = flox.cci(highs, lows, closes, period=20)
```

---

## Trend

### `adx(high, low, close, period=14) -> dict`

Average Directional Index with directional indicators.

```python
result = flox.adx(highs, lows, closes, period=14)
# result['adx'], result['plus_di'], result['minus_di']
```

### `chop(high, low, close, period=14) -> ndarray`

Choppiness Index (0–100). High values indicate ranging markets.

```python
result = flox.chop(highs, lows, closes, period=14)
```

### `slope(input, length=1) -> ndarray`

Linear slope over a lookback window.

```python
result = flox.slope(closes, length=5)
```

---

## Volatility

### `atr(high, low, close, period) -> ndarray`

Average True Range.

```python
result = flox.atr(highs, lows, closes, period=14)
```

### `bollinger(input, period=20, stddev=2.0) -> dict`

Bollinger Bands.

```python
result = flox.bollinger(closes, period=20, stddev=2.0)
# result['upper'], result['middle'], result['lower']
```

---

## Statistical

### `skewness(input, period) -> ndarray`

Rolling Fisher-Pearson skewness. Measures distribution asymmetry. Requires period >= 3. NaN if std = 0.

```python
result = flox.skewness(closes, period=20)
```

### `kurtosis(input, period) -> ndarray`

Rolling Fisher excess kurtosis. Measures tail heaviness. Requires period >= 4. NaN if std = 0.

```python
result = flox.kurtosis(closes, period=20)
```

### `rolling_zscore(input, period) -> ndarray`

Rolling z-score normalization: `(x - mean) / std`. NaN if std = 0.

```python
result = flox.rolling_zscore(closes, period=20)
```

### `shannon_entropy(input, period, bins=10) -> ndarray`

Rolling Shannon entropy with histogram binning, normalized to [0, 1]. Zero = all values identical, 1 = uniform distribution.

```python
result = flox.shannon_entropy(closes, period=20, bins=10)
```

### `parkinson_vol(high, low, period) -> ndarray`

Parkinson high-low volatility estimator: `sqrt(mean(ln(H/L)^2) / (4*ln(2)))`. More efficient than close-to-close volatility.

```python
result = flox.parkinson_vol(highs, lows, period=20)
```

### `rogers_satchell_vol(open, high, low, close, period) -> ndarray`

Rogers-Satchell OHLC volatility estimator. Unbiased with drift, suitable for trending markets.

```python
result = flox.rogers_satchell_vol(opens, highs, lows, closes, period=20)
```

### `correlation(x, y, period) -> ndarray`

Rolling Pearson correlation between two series. NaN if either series is constant within the window.

```python
result = flox.correlation(closes_btc, closes_eth, period=20)
```

---

## Volume

### `vwap(close, volume, window=96) -> ndarray`

Rolling Volume-Weighted Average Price.

```python
result = flox.vwap(closes, volumes, window=96)
```

### `obv(close, volume) -> ndarray`

On-Balance Volume.

```python
result = flox.obv(closes, volumes)
```

### `cvd(open, high, low, close, volume) -> ndarray`

Cumulative Volume Delta. Estimates buying/selling pressure from OHLCV data.

```python
result = flox.cvd(opens, highs, lows, closes, volumes)
```

---

## Strategy Helpers

### `bar_returns(signal_long, signal_short, log_returns) -> ndarray`

Compute per-bar returns given position signals and log returns.

```python
returns = flox.bar_returns(signal_long, signal_short, log_returns)
```

`signal_long` and `signal_short` are `int8[]` (+1 or 0 / -1 or 0). `log_returns` is `float64[]`.

### `trade_pnl(signal_long, signal_short, log_returns) -> ndarray`

Extract per-trade PnL from position signals.

```python
pnls = flox.trade_pnl(signal_long, signal_short, log_returns)
```

### `profit_factor(returns) -> float`

Ratio of gross profit to gross loss. Values > 1.0 indicate profitability.

```python
pf = flox.profit_factor(returns)
```

### `win_rate(trade_pnls) -> float`

Fraction of trades with positive PnL.

```python
wr = flox.win_rate(pnls)
```
