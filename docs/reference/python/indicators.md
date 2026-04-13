# Indicators

Vectorized technical indicators operating on numpy arrays. All functions release the GIL for parallel computation.

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
macd_line = result['line']
signal_line = result['signal']
histogram = result['histogram']
```

| Key | Description |
|-----|-------------|
| `line` | MACD line (fast EMA - slow EMA) |
| `signal` | Signal line (EMA of MACD line) |
| `histogram` | MACD - Signal |

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
adx_values = result['adx']
plus_di = result['plus_di']
minus_di = result['minus_di']
```

| Key | Description |
|-----|-------------|
| `adx` | ADX values |
| `plus_di` | +DI (positive directional indicator) |
| `minus_di` | -DI (negative directional indicator) |

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
upper = result['upper']
middle = result['middle']
lower = result['lower']
```

| Key | Description |
|-----|-------------|
| `upper` | Upper band (middle + stddev * std) |
| `middle` | Middle band (SMA) |
| `lower` | Lower band (middle - stddev * std) |

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

| Parameter | Type | Description |
|-----------|------|-------------|
| `signal_long` | `int8[]` | Long position flags (+1 or 0) |
| `signal_short` | `int8[]` | Short position flags (-1 or 0) |
| `log_returns` | `float64[]` | Log returns of the asset |

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
