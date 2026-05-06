"""Example: use the indicator inside a FLOX strategy.

Run after ``pip install -e .[dev]`` in this project root::

    python examples/use_in_strategy.py
"""
from __future__ import annotations

import os
from pathlib import Path

import flox_py as flox

from __PROJECT_SLUG__ import ZLEMA


HERE = Path(__file__).resolve().parent
SAMPLE_CSV = HERE.parent / "tests" / "data" / "btcusdt_sample.csv"


registry = flox.SymbolRegistry()
btc = registry.add_symbol("exchange", "BTCUSDT", tick_size=0.01)


class ZlemaCrossStrategy(flox.Strategy):
    """Buy when fast ZLEMA crosses above slow ZLEMA, exit on the reverse."""

    def __init__(self, symbols, *, fast: int = 10, slow: int = 30,
                 qty: float = 0.01):
        super().__init__(symbols)
        self.fast = ZLEMA(fast)
        self.slow = ZLEMA(slow)
        self.qty = qty

    def on_trade(self, ctx, trade):
        f = self.fast.update(trade.price)
        s = self.slow.update(trade.price)
        if f is None or s is None:
            return
        if f > s and ctx.is_flat():
            self.market_buy(self.qty)
        elif f < s and ctx.is_flat():
            self.market_sell(self.qty)


def main() -> None:
    if not SAMPLE_CSV.exists():
        raise SystemExit(f"sample CSV not found at {SAMPLE_CSV}")
    bt = flox.BacktestRunner(registry, fee_rate=0.0004,
                             initial_capital=10_000)
    bt.set_strategy(ZlemaCrossStrategy([btc]))
    stats = bt.run_csv(str(SAMPLE_CSV), symbol="BTCUSDT")
    print(f"backtest on {SAMPLE_CSV.name}")
    print(f"  return : {stats['return_pct']:+.4f}%")
    print(f"  trades : {stats['total_trades']}  "
          f"win={stats['win_rate'] * 100:.1f}%")
    print(f"  sharpe : {stats['sharpe']:.4f}")
    print(f"  max DD : {stats['max_drawdown_pct']:.4f}%")


if __name__ == "__main__":
    main()
