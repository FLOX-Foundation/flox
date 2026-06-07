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

The interface also clones. Sizing a swap to a target price, the core of arb and
inventory-aware quoting, is a search over swap size, and each trial needs a
post-swap state to read back without committing it. The non-mutating queries
answer the output and the impact of a swap but not the price it leaves behind,
so `clone()` gives that search a throwaway copy: bisect the swap size on a clone,
then apply the winner to the live curve. It works over `IAmmCurve` for any
model, so arb and routing code does not care which curve it holds.

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

## Stableswap

A stableswap pool (Curve style) is built for assets that trade near a peg, like
two dollar stablecoins. Its invariant blends constant-sum, which gives a flat
one-for-one price, with constant-product, which keeps the pool from ever
emptying. An amplification coefficient `A` sets where the blend tips: a high `A`
holds the price near 1 across a wide band of balances and only curves sharply
once the pool is far from balance. So a swap near the peg slips far less than it
would on a constant-product pool of the same size.

`StableSwapCurve` holds the two balances, `A`, and the fee. Neither the
invariant `D` nor the swap output is closed-form: `D` comes from a Newton
iteration over the balances, and the output balance from a second Newton solve
of a quadratic against the new input balance. Both converge in a handful of
steps. The state a backtest carries is still just two balances plus `A`, but
each quote costs two small solves instead of one division.

## Cryptoswap

A cryptoswap pool (Curve V2) is the volatile-asset counterpart to stableswap.
The two assets have no peg, so a fixed flat zone would be wrong, but the pool
still wants stableswap-like depth around wherever the price currently sits. It
gets that by making the blend coefficient `K` depend on how balanced the pool
is: near balance `K` is large and the curve is flat, and as the pool drifts off
balance `K` decays toward zero and the curve falls back to constant-product. A
sets the depth near balance, gamma how fast that depth fades as the pool moves.
The result sits between stableswap and constant-product: flatter than a Uniswap
pool near balance, but never as flat as a true stableswap once the price moves.

`CryptoswapCurve` holds the two balances, A, gamma, and the fee. The invariant
is non-monotonic, so it is not solved by plain Newton: both the invariant value
and the swap output come from a safeguarded solve, a Newton step that falls back
to bisection whenever the step would leave the bracketed physical branch. The
state a backtest carries is still two balances plus the two parameters.

## Pools with more than two tokens

Every curve so far is two-token: a base and a quote, with `baseForQuote`
picking the direction. That covers each family in its pair form, and most pools
trade as a pair. Some do not. A Balancer pool can hold eight assets at once, a
Curve stable pool three or more, a tricrypto pool three; a swap there names an
in-token and an out-token out of the whole set, and the price between them
depends on every balance in the pool, not just two.

`INTokenCurve` is the interface for those. It indexes tokens `[0, tokenCount)`
and prices between an ordered pair: `spotPrice(i, j)`, `amountOut(i, j, in)`,
`priceImpact(i, j, in)`, `applySwap(i, j, in)`, `balances()`, and `clone()`. The
balances are on the interface because valuing an LP position and accounting for
impermanent loss both need the pool's composition, and every multi-asset pool
has one. It is a sibling
of `IAmmCurve`, not a replacement: the two-token curves and everything that
consumes them stay exactly as they are, and a pool that genuinely holds more
than two tokens implements this instead. Forcing the n-token case onto the
base/quote interface would distort both, so they sit side by side.

## Weighted baskets

A Balancer-style weighted pool generalizes cleanly to many assets because its
swap formula only ever looks at the in-token and the out-token. The output for
swapping token i into token j is the same closed form as the two-token weighted
curve, `B_j·(1 − (B_i/(B_i+inFee))^(w_i/w_j))`, and the other balances in the
pool play no part in that swap. So an eight-asset pool is not eight-way coupled
math: it is the two-token formula applied to whichever pair a swap touches, with
the rest of the basket sitting idle.

`WeightedPoolN` holds n balances, n weights summing to 1, and a fee, and prices
any pair `(i, j)`. A two-asset `WeightedPoolN` is the same numbers as a
`WeightedCurve`, and equal weights reduce a pair to constant-product, the same
way they do for the two-token curve.

## Stable baskets

A Curve stable pool can hold three or more pegged coins in one curve, a 3pool of
stablecoins being the common case. It is the same blend of constant-sum and
constant-product as the two-coin stableswap, widened to n coins: the invariant D
and the swap output still come from Newton iteration, but the loops now run over
every balance in the pool, not two.

`StableSwapPoolN` holds n balances, the amplification `A`, and the fee, and
prices any pair `(i, j)` by solving `get_y` for coin j against the bumped
balance of coin i while the other coins hold still. A two-coin `StableSwapPoolN`
is the same numbers as a `StableSwapCurve`. Near the peg it fills flatter than
constant-product on any pair, and a lower `A` curves earlier, exactly as in the
two-coin case.

## Volatile baskets

A Curve V2 cryptoswap pool (a tricrypto pool) holds several volatile assets in
one curve. It is the n-coin form of the two-coin cryptoswap: the same
superposition of constant-product and stableswap, run in transformed balances.
Coin 0 is the numeraire and every other coin is scaled into it through a
price-scale vector, `xp[k] = balance[k]·price_scale[k-1]`, and the invariant is
solved in that space. The pool fills flatter than constant-product near balance
and falls back toward it once a pair drifts off balance, the same way the
two-coin curve does.

`CryptoswapPoolN` holds n balances, the price-scale vector, `A`, `gamma`, and a
fee. The invariant is non-monotonic and has more than one root, so solving for
the output coin is not a plain Newton step: the physical root is the topmost one
below the coin's current balance, found by bracketing the first sign change and
solving inside that bracket. A two-coin `CryptoswapPoolN` with unit scale is the
same numbers as a `CryptoswapCurve`. Here the price scale is fixed; the
repegging that moves it is the next piece.

## Repegging

The static cryptoswap pool concentrates liquidity at a fixed price scale. A real
Curve V2 pool moves that scale to follow the traded price, with no external
oracle, so the liquidity stays where the trading is. `RepeggingCryptoswapPool`
adds that on top of the static pool, with two parts.

The fee becomes dynamic. It is `mid_fee` when the pool is balanced and climbs
toward `out_fee` as the pool tips, blended by `fee_gamma`. A lopsided pool
charges more, which is both a deterrent and the fee income that pays for
rebalancing.

Then the repeg itself. After each swap an internal EMA oracle tracks the new
marginal price. When the oracle has drifted from the scale by more than
`adjustment_step`, and the pool's virtual price shows it is far enough ahead to
afford it, the scale takes a step toward the oracle. The step is computed, the
invariant and virtual price are recomputed at the new scale, and the move is
kept only if it leaves the pool in profit; otherwise it is reverted. So a
rebalance never spends the LPs' fee income, and `xcp_profit`, the running fee
profit, only grows. Each swap advances the oracle by one time step, with
`ma_half_time` measured in those steps.

## What it does not touch

The CLOB SimulatedExecutor is unchanged. A centralized-exchange backtest fills
against the order book; only an AMM venue fills through a curve. The two
pricing models sit side by side, chosen by venue type. Where a backtest sources
its curve state from is the concern of the connector that drives the DEX venue,
not of the curve itself.
