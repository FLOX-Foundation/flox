"""__PROJECT_NAME__ — FLOX research scaffold.

Edit this file to plug in your own indicators, signals, and execution
logic. The template ships with an SMA(10/30) crossover on BTCUSDT to
keep the round-trip short: clone, run, get a backtest report.

Run::

    pip install -r requirements.txt
    python main.py
"""
from __future__ import annotations

import os
import time

import flox_py as flox


# Replace with your own CSV (timestamp_ms,price,qty,is_buyer_maker).
# An empty path triggers the synthetic price path below — handy for
# the very first run before you wire up real data.
DATA_CSV = os.environ.get("__PROJECT_ENV__", "")


registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)


class __PROJECT_SLUG___strategy(flox.Strategy):
    """SMA(10/30) crossover — replace with your own logic."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = flox.SMA(10)
        self.slow = flox.SMA(30)
        self.trade_count = 0

    def on_start(self):
        print("  __PROJECT_NAME__ strategy started")

    def on_stop(self):
        print(f"  __PROJECT_NAME__ stopped  ({self.trade_count} signals)")

    def on_trade(self, ctx, trade):
        fv = self.fast.update(trade.price)
        sv = self.slow.update(trade.price)
        if fv is None or sv is None or not self.slow.ready:
            return
        if fv > sv and ctx.is_flat():
            self.market_buy(0.01)
            self.trade_count += 1
        elif fv < sv and ctx.is_flat():
            self.market_sell(0.01)
            self.trade_count += 1


def run_backtest() -> None:
    print("-- backtest -------------------------------------------------")
    strat = __PROJECT_SLUG___strategy([btc])
    bt = flox.BacktestRunner(registry, fee_rate=0.0004,
                             initial_capital=10_000)
    bt.set_strategy(strat)
    stats = bt.run_csv(DATA_CSV)
    print(f"  return : {stats['return_pct']:+.4f}%")
    print(f"  trades : {stats['total_trades']}  "
          f"win={stats['win_rate']*100:.1f}%")
    print(f"  sharpe : {stats['sharpe']:.4f}")
    print(f"  max DD : {stats['max_drawdown_pct']:.4f}%")


def run_synthetic() -> None:
    print("-- synthetic live ------------------------------------------")
    signals: list[flox.Signal] = []
    runner = flox.Runner(registry, signals.append)
    runner.add_strategy(__PROJECT_SLUG___strategy([btc]))
    runner.start()

    ts_ns = int(time.time()) * 1_000_000_000
    for i in range(60):
        price = 50_000 + i * 50
        runner.on_trade(btc, float(price), 0.1, i % 2 == 0, ts_ns)
        ts_ns += 1_000_000_000

    runner.stop()
    print(f"  signals: {len(signals)}")


if __name__ == "__main__":
    if DATA_CSV and os.path.exists(DATA_CSV):
        run_backtest()
    else:
        print(f"  (set __PROJECT_ENV__=<csv> for backtest mode)")
        run_synthetic()
