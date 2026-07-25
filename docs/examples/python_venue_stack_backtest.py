"""Run a BacktestRunner against a VenueStack's fill mechanics.

The runner feeds the stack's executor market data and harvests its fills,
so the run uses that venue's queue model and depth, iceberg refresh
latency, venue availability and rate limits.

Two things this does not change: fees still come from BacktestConfig
(Fill carries no maker/taker flag, so the stack's tiered FeeSchedule
cannot be applied per fill), and funding and liquidation are driven by
explicit calls rather than the replay loop. Read the result as "this
venue's fill mechanics", not "this venue's full economics".
"""
import pathlib
from collections import deque

import flox_py as flox

CSV = str(
    pathlib.Path(flox.__file__).resolve().parent
    / "templates/research/data/btcusdt_sample.csv"
)


class CrossAboveSMA(flox.Strategy):
    def __init__(self, symbols, period=20):
        super().__init__(symbols)
        self.window = deque(maxlen=period)

    def on_trade(self, ctx, trade):
        self.window.append(trade.price)
        if len(self.window) < self.window.maxlen:
            return
        sma = sum(self.window) / len(self.window)
        if trade.price > sma and ctx.is_flat():
            self.market_buy(1.0)
        elif trade.price < sma and ctx.is_long():
            self.close_position()


def main():
    registry = flox.SymbolRegistry()
    btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

    bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(CrossAboveSMA([btc]))

    stack = flox.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
    bt.set_venue_stack(stack)  # pass None to revert to the built-in executor

    stats = bt.run_csv(CSV, "BTCUSDT")

    print(f"trades      : {stats['total_trades']}")
    print(f"return      : {stats['return_pct']:.2f}%")
    print(f"venue fills : {len(stack.executor().fills_list())}")


if __name__ == "__main__":
    main()
