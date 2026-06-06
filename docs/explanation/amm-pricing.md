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

## The curve interface

A pricing curve answers four questions: the spot price, the output amount for
a swap of a given size and direction, the price impact of that swap, and the
new state after applying it. `IAmmCurve` is that interface, and the connector,
the backtest pricing, and the LP valuator work over it without knowing which
model is underneath. A swap that applies returns its output and moves the
curve's state, so a sequence of swaps in a backtest sees the pool drift the way
a real one would.

`ConstantProductCurve` is the simplest implementation: it holds the two
reserves and the fee. Other models implement the same interface, each with its
own state: weighted pools, concentrated liquidity, stableswap, cryptoswap. A
backtest swaps one curve for another without touching the rest.

## Weighted pools

A weighted pool (Balancer style) keeps `B_base^w_base * B_quote^w_quote`
constant, with the two weights summing to 1. Equal weights reduce it to
constant-product; an unequal split prices the heavier-weighted token higher for
the same balance, so an 80/20 pool with equal balances quotes the 80% token at
four times the 20% token. The spot price is `(B_quote/w_quote)/(B_base/w_base)`
and the output is closed-form:

    amountOut = B_out * (1 - (B_in / (B_in + amountIn*(1-fee)))^(w_in/w_out))

`WeightedCurve` needs only the balances, the weights, and the fee, plus a
power. It is a closed-form curve like constant-product, no solver.

## Concentrated liquidity

A concentrated-liquidity pool (Uniswap v3 style, also Orca and Raydium on
Solana) places liquidity in price ranges instead of across the whole curve.
The active liquidity `L` is constant only between two adjacent initialized
ticks; inside a range the curve is constant-product on the virtual reserves
`L/sqrt(P)` and `L*sqrt(P)`, and crossing a tick changes `L` by that tick's
liquidity. A swap large enough to leave a range is not a single closed-form
step: it walks the ticks, consuming each range to its boundary, crossing into
the next, until the input is spent or the liquidity runs out.

`ConcentratedLiquidityCurve` holds the current price, the active liquidity, and
the tick table, and walks the swap across boundaries. The state a backtest must
carry is therefore the tick and liquidity map, not just two reserves, and a
swap can exhaust the available liquidity and fill less than its full size. This
is the dominant model by 2026 DEX spot volume.

## What it does not touch

The CLOB SimulatedExecutor is unchanged. A centralized-exchange backtest fills
against the order book; only an AMM venue fills through a curve. The two
pricing models sit side by side, chosen by venue type. Where a backtest sources
its curve state from is the concern of the connector that drives the DEX venue,
not of the curve itself.
