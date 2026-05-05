"""__PROJECT_NAME__ — FLOX research scaffold.

Edit this file. The ``__PROJECT_SLUG___strategy`` class below runs an
SMA(10/30) crossover on the bundled ``data/btcusdt_sample.csv`` (500
real BTC 1m bars). To use your own data, set the env var
``__PROJECT_ENV__`` to a CSV path with columns
``timestamp,open,high,low,close,volume``.

Run::

    pip install -r requirements.txt
    python main.py
"""
from __future__ import annotations

import os

import flox_py as flox


HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CSV = os.path.join(HERE, "data", "btcusdt_sample.csv")
DATA_CSV = os.environ.get("__PROJECT_ENV__", DEFAULT_CSV)


registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)


class __PROJECT_SLUG___strategy(flox.Strategy):
    """SMA(10/30) crossover. Replace with your own logic."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = flox.SMA(10)
        self.slow = flox.SMA(30)

    def on_trade(self, ctx, trade):
        fv = self.fast.update(trade.price)
        sv = self.slow.update(trade.price)
        if fv is None or sv is None or not self.slow.ready:
            return
        if fv > sv and ctx.is_flat():
            self.market_buy(0.01)
        elif fv < sv and ctx.is_flat():
            self.market_sell(0.01)


def main() -> None:
    if not os.path.exists(DATA_CSV):
        raise SystemExit(
            f"data file not found: {DATA_CSV}\n"
            f"set __PROJECT_ENV__=<path-to-csv> or restore "
            f"data/btcusdt_sample.csv"
        )

    print(f"backtest on {os.path.basename(DATA_CSV)}")
    bt = flox.BacktestRunner(registry, fee_rate=0.0004,
                             initial_capital=10_000)
    bt.set_strategy(__PROJECT_SLUG___strategy([btc]))
    stats = bt.run_csv(DATA_CSV, symbol="BTCUSDT")
    print(f"  return : {stats['return_pct']:+.4f}%")
    print(f"  trades : {stats['total_trades']}  "
          f"win={stats['win_rate']*100:.1f}%")
    print(f"  sharpe : {stats['sharpe']:.4f}")
    print(f"  max DD : {stats['max_drawdown_pct']:.4f}%")
    print(f"  net PnL: {stats['net_pnl']:.4f}")


if __name__ == "__main__":
    main()
