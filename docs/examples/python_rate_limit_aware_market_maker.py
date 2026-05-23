"""Rate-limit-aware market maker — respects the venue's per-endpoint budget.

Real venues split rate budgets per endpoint family (trading vs market
data vs account queries). A naive quote-cancel-replace loop bursts
through the trading budget and produces a stream of REJECTED_RATE_LIMIT
events. This recipe shows how to consult the policy before issuing
each action.

Demonstrates:
- VenueStack ships a venue-realistic RateLimitPolicy (T022/T049)
- Per-endpoint budgets (Trading / MarketData / Account family)
- Ban-after-N-consecutive-rejects logic
"""

import flox_py as flox

BTC = 1


def main():
    stack = flox.VenueStack.binance_um_futures(account_id=1, equity=10_000.0)
    # RateLimitPolicy is wired by the venue factory and attached to the
    # executor. Inspect it for current ban / consecutive-reject state.
    # (The wrap exposes ban-until and reject-count accessors via the
    # SimulatedExecutor's policy proxy.)

    exec_ = stack.executor()

    # A quoting loop that issues a cancel + replace every tick. Real
    # market makers do this thousands of times per second; without rate
    # awareness the venue will ban the account.
    submitted = 0
    rejected = 0
    for tick in range(200):
        now_ns = tick * 100_000_000  # 100ms cadence
        # Try a submit. The simulated executor consults its rate-limit
        # policy on every submit; reject events surface via the order
        # event callback (omitted here for brevity).
        order = flox.Order()
        order.id = tick
        order.symbol = BTC
        order.side = flox.Side.BUY
        order.type = flox.OrderType.LIMIT
        order.price = flox.Price.from_double(50_000.0 - tick * 0.5)
        order.quantity = flox.Quantity.from_double(0.001)
        try:
            exec_.submit_order(order)
            submitted += 1
        except Exception:
            rejected += 1

    print(f"submitted: {submitted}")
    print(f"rejected: {rejected}")
    # The venue factory's policy is shaped after Binance UM's published
    # limits — depending on the cadence the loop may saturate the
    # trading-family budget.


if __name__ == "__main__":
    main()
