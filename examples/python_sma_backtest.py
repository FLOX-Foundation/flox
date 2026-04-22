"""
SMA crossover backtest — BTCUSDT 1m data.

Demonstrates the Python flox_py API:
  - Engine: load CSV, get OHLCV arrays
  - Batch indicators: rsi, bollinger, macd, atr
  - Streaming SMA for signal generation
  - SignalBuilder: accumulate buy/sell signals
  - Engine.run(): replay signals against bars, return BacktestStats

Usage:
    cd /path/to/flox
    PYTHONPATH=build/python python3 examples/python_sma_backtest.py
"""

import os
import time
import flox_py as flox

DATA = os.path.join(os.path.dirname(__file__), "data", "btcusdt_1m.csv")

# ── Load data ─────────────────────────────────────────────────────────

engine = flox.Engine(initial_capital=10_000, fee_rate=0.0004)
engine.load_csv(DATA)

ts    = engine.ts()      # int64 ns timestamps
close = engine.close()
high  = engine.high()
low   = engine.low()
n     = len(close)

print(f"Loaded {n} bars  {close[0]:.2f} → {close[-1]:.2f}")

# ── Batch indicators ──────────────────────────────────────────────────

rsi  = flox.rsi(close, 14)
bb   = flox.bollinger(close, 20, 2.0)
macd = flox.macd(close, 12, 26, 9)
atr  = flox.atr(high, low, close, 14)

print(f"RSI={rsi[-1]:.1f}  "
      f"BB={bb['lower'][-1]:.0f}/{bb['middle'][-1]:.0f}/{bb['upper'][-1]:.0f}  "
      f"MACD={macd['line'][-1]:.2f}  ATR={atr[-1]:.2f}")

# ── Signal generation — SMA(10/30) crossover ─────────────────────────

fast_sma = flox.SMA(10)
slow_sma = flox.SMA(30)
signals  = flox.SignalBuilder()
pos      = 0   # -1=short  0=flat  1=long

for i in range(n):
    fv = fast_sma.update(close[i])
    sv = slow_sma.update(close[i])
    if not slow_sma.ready:
        continue
    if fv > sv and pos <= 0:
        # Cross up: close short (if any) + go long
        signals.buy(int(ts[i]), 0.01 if pos == 0 else 0.02)
        pos = 1
    elif fv < sv and pos >= 0:
        # Cross down: close long (if any) + go short
        signals.sell(int(ts[i]), 0.01 if pos == 0 else 0.02)
        pos = -1

print(f"\n{len(signals)} signals generated")

# ── Run backtest ──────────────────────────────────────────────────────

t0    = time.perf_counter_ns()
stats = engine.run(signals)
dt    = (time.perf_counter_ns() - t0) / 1e6

print(f"\nSMA(10/30) crossover results")
print(f"  Capital  : {stats.initial_capital:,.2f} → {stats.final_capital:,.2f}")
print(f"  Return   : {stats.return_pct:+.4f}%")
print(f"  Trades   : {stats.total_trades}  (win rate {stats.win_rate*100:.1f}%)")
print(f"  Prof.factor: {stats.profit_factor:.2f}")
print(f"  Sharpe   : {stats.sharpe:.4f}")
print(f"  Max DD   : {stats.max_drawdown_pct:.4f}%")
print(f"  Fees     : {stats.total_fees:.4f}")
print(f"  PnL      : {stats.net_pnl:.4f}")
print(f"  ({dt:.2f} ms)")
