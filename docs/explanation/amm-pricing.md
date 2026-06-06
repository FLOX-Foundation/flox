# AMM pricing in backtests

A central-limit-order-book backtest fills an order by walking price levels.
An AMM venue has no book. A swap fills against the pool's reserves, and the
price it gets depends on the size of the swap relative to those reserves. To
backtest DEX execution honestly, the fill has to come from the pool curve,
not from a book.

## The constant-product curve

A Uniswap v2 style pool holds two reserves and keeps their product constant
across a swap. Putting an amount in moves the reserves along that curve, and
the output is whatever keeps the product fixed after the pool fee is taken:

    amountInWithFee = amountIn * (1 - fee)
    amountOut = reserveOut * amountInWithFee / (reserveIn + amountInWithFee)

Two consequences fall out of this and matter for a backtest. A larger swap
gets a worse average rate, because it moves the reserves further. And the
realized rate is always below the spot rate, by an amount that grows with
size. That gap is the price impact, and ignoring it makes a DEX strategy look
more profitable than it is.

## What AmmPool provides

`AmmPool` holds the reserves and the fee tier and answers three questions:
the spot price, the output amount for a swap of a given size and direction,
and the price impact of that swap. Applying a swap returns the output and
moves the reserves, so a sequence of swaps in a backtest sees the pool drift
the way a real pool would.

## What it does not touch

The CLOB SimulatedExecutor is unchanged. A centralized-exchange backtest
still fills against the order book; only an AMM venue fills through this
curve. The two pricing models sit side by side, chosen by venue type.

This is the v2 constant-product curve. A v3 concentrated-liquidity pool,
where liquidity is placed in ranges and the effective reserves change as
price crosses a range boundary, is a later addition. Where a backtest sources
its reserve history from is the concern of the connector that drives the DEX
venue, not of the curve itself.
