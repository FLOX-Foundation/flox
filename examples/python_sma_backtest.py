"""
SMA Crossover Backtest on real Binance BTCUSDT 1m data.

Usage:
    cd /Users/e/dev/flox
    PYTHONPATH=build/python .venv/bin/python3 examples/python_sma_backtest.py
"""

import os
import time
import numpy as np
import flox_py as flox

DATA = os.path.join(os.path.dirname(__file__), "data", "btcusdt_1m.csv")


def sma_signals(ts, close, fast=10, slow=30, size=0.01):
    f, s = flox.SMA(fast), flox.SMA(slow)
    sig = flox.SignalBuilder()
    pos = 0

    for i in range(len(close)):
        fv, sv = f.update(close[i]), s.update(close[i])
        if not s.ready:
            continue
        if fv > sv and pos <= 0:
            sig.buy(int(ts[i]), size if pos == 0 else size * 2)
            pos = 1
        elif fv < sv and pos >= 0:
            sig.sell(int(ts[i]), size if pos == 0 else size * 2)
            pos = -1

    return sig.build()


if __name__ == "__main__":
    engine = flox.Engine(initial_capital=10_000, fee_rate=0.0004)
    engine.load_csv(DATA)

    ts, close, high, low = engine.ts, engine.close, engine.high, engine.low
    n = len(close)
    print(f"{n} bars  {close[0]:.2f} -> {close[-1]:.2f}  [{close.min():.2f}, {close.max():.2f}]")

    # indicators
    rsi = flox.rsi(close, 14)
    bb = flox.bollinger(close, 20, 2.0)
    macd = flox.macd(close, 12, 26, 9)
    atr = flox.atr(high, low, close, 14)
    print(f"\nRSI={rsi[-1]:.1f}  BB={bb['lower'][-1]:.0f}/{bb['middle'][-1]:.0f}/{bb['upper'][-1]:.0f}"
          f"  MACD={macd['line'][-1]:.1f}  ATR={atr[-1]:.1f}")

    # backtest
    signals = sma_signals(ts, close)
    print(f"{len(signals)} signals")

    t0 = time.perf_counter_ns()
    stats = engine.run(signals)
    dt = (time.perf_counter_ns() - t0) / 1e6

    print(f"\n{stats}")
    print(f"  {stats.initial_capital:,.2f} -> {stats.final_capital:,.2f}  {stats.return_pct:+.4f}%")
    print(f"  trades={stats.total_trades} wr={stats.win_rate*100:.1f}% pf={stats.profit_factor:.2f}")
    print(f"  sharpe={stats.sharpe:.4f} dd={stats.max_drawdown_pct:.4f}% fees={stats.total_fees:.4f}")
    print(f"  ({dt:.2f}ms)")

    # permutation test
    rng = np.random.default_rng(42)
    log_ret = np.diff(np.log(close))
    sets = []
    for _ in range(100):
        perm = log_ret.copy()
        rng.shuffle(perm)
        fake = close[0] * np.exp(np.cumsum(np.r_[0, perm]))
        sets.append(sma_signals(ts, fake))

    t0 = time.perf_counter_ns()
    batch = engine.run_batch(sets)
    dt = (time.perf_counter_ns() - t0) / 1e6

    pnls = np.array([r.net_pnl for r in batch])
    p = np.mean(pnls >= stats.net_pnl)
    print(f"\npermutation test: p={p:.4f} ({dt:.2f}ms)")
