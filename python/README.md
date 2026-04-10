# flox-py

Python bindings for the Flox indicator library. Tested against TA-Lib.

## Install

```bash
pip install flox-py
```

## Usage

```python
import flox_py as flox
import numpy as np

close = np.array([...])
high = np.array([...])
low = np.array([...])

ema50 = flox.ema(close, 50)
atr14 = flox.atr(high, low, close, 14)
rsi14 = flox.rsi(close, 14)

signal_long = (rsi14 > 70).astype(np.int8)
signal_short = ((rsi14 < 30) * -1).astype(np.int8)

returns = flox.bar_returns(signal_long, signal_short, log_returns)
trades = flox.trade_pnl(signal_long, signal_short, log_returns)
pf = flox.profit_factor(returns)
wr = flox.win_rate(trades)
```

## Indicators

| Function | Description |
|----------|-------------|
| `flox.ema(input, period)` | Exponential Moving Average |
| `flox.sma(input, period)` | Simple Moving Average |
| `flox.rma(input, period)` | Wilder Moving Average |
| `flox.rsi(input, period)` | Relative Strength Index |
| `flox.atr(high, low, close, period)` | Average True Range |
| `flox.adx(high, low, close, period)` | ADX, +DI, -DI |
| `flox.macd(input, fast, slow, signal)` | MACD line, signal, histogram |
| `flox.bollinger(input, period, stddev)` | upper, middle, lower bands |
| `flox.stochastic(high, low, close, k, d)` | %K and %D |
| `flox.cci(high, low, close, period)` | Commodity Channel Index |
| `flox.slope(input, length)` | price slope |
| `flox.kama(input, period)` | Kaufman Adaptive MA |
| `flox.dema(input, period)` | Double EMA |
| `flox.tema(input, period)` | Triple EMA |
| `flox.chop(high, low, close, period)` | Choppiness Index |
| `flox.obv(close, volume)` | On-Balance Volume |
| `flox.vwap(close, volume, window)` | rolling VWAP |
| `flox.cvd(open, high, low, close, volume)` | Cumulative Volume Delta |

## Metrics

| Function | Description |
|----------|-------------|
| `flox.bar_returns(sig_long, sig_short, log_ret)` | per-bar returns with signal shift |
| `flox.trade_pnl(sig_long, sig_short, log_ret)` | per-trade PnL array |
| `flox.profit_factor(returns)` | sum positive / abs sum negative |
| `flox.win_rate(trade_pnls)` | fraction of winning trades |
