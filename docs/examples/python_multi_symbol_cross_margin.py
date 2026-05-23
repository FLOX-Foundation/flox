"""Multi-symbol cross-margin backtest with on_marks + stale-mark guard.

A profitable BTC short backs a losing ETH long via shared account
equity. Demonstrates the atomic multi-symbol mark update path (T053)
and the stale-mark guard that catches silent footguns where the
caller forgets to refresh one symbol's mark before walking
liquidations.

Exercises:
- VenueStack factory (T052)
- Cross-margin Account (T037)
- on_marks atomic multi-symbol update (T053)
- Stale-mark guard for liquidation safety
"""

import flox_py as flox

BTC = 1
ETH = 2


def main():
    stack = flox.VenueStack.binance_um_futures(account_id=7, equity=50_000.0)
    acct = stack.account()
    liq = stack.liquidation()

    # BTC short profits as the market drops; ETH long bleeds.
    acct.open_position(symbol=BTC, quantity=-0.5, entry_price=50_000.0)
    acct.open_position(symbol=ETH, quantity=10.0, entry_price=3_000.0)

    # 1-minute budget for stale marks. Anything older flags as stale.
    stale_budget_ns = 60 * 1_000_000_000

    for i in range(10):
        ts = i * 5 * 60 * 1_000_000_000  # 5-minute ticks

        # Refresh BOTH symbols atomically. The on_marks call updates
        # every attached account's marks before walking, so the
        # cross-margin check sees both fresh prices in one pass.
        marks = [
            (BTC, 50_000.0 - 200.0 * i),  # BTC dropping
            (ETH, 3_000.0 - 30.0 * i),    # ETH dropping
        ]

        # Sanity check: refuse to walk if any position would be
        # evaluated against stale data. Should never fire here since
        # we just refreshed both symbols.
        if acct.has_stale_marks(now_ns=ts, budget_ns=stale_budget_ns):
            raise RuntimeError(f"tick {i}: stale marks; refresh before walking")

        out = liq.on_marks(marks, ts_ns=ts)
        print(f"tick {i}: liq={out['liquidations_count']} "
              f"upnl={acct.total_unrealised_pnl():.2f}")

    print(f"final equity: {acct.equity():.2f}")
    print(f"final positions: {acct.position_count()}")


if __name__ == "__main__":
    main()
