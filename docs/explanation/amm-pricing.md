# AMM pricing in backtests

A central-limit-order-book backtest fills an order by walking price levels. An
AMM venue has no book: a swap fills against the pool's balances, and the price
it gets depends on the size of the swap relative to those balances. To backtest
DEX execution honestly, the fill has to come from the pool curve.

## Exact, in native-wei integers

The deployed pool contracts compute in `uint256` over native token units (wei),
with floor division and their own rounding. A double model of the same curve is
close but never bit-exact: double rounds to nearest while the contract floors,
and in a sequential backtest that small difference compounds. For anything
touching real money against a real pool, the on-chain quote is the source of
truth, so the curves here reproduce the contract's integer arithmetic and match
it to the wei.

The amounts are `u256`, an exact unsigned 256-bit integer, in the token's own
units. There is no double approximation behind a curve. Converting to the
engine's `Quantity` happens once, at the boundary where a curve result becomes
an engine event, not on the curve itself.

## The curve interface

`INTokenCurve` is the one curve interface. A pool holds tokens indexed
`[0, tokenCount)` and prices a swap between an in-token `i` and an out-token `j`:
`amountOut(i, j, amountIn)` returns the exact output, `applySwap` returns it and
moves the pool, `balances()` exposes the composition, and `clone()` makes an
independent copy for sizing a swap without disturbing the live pool. A two-token
pool is just `n = 2`, so there is no separate two-token interface.

## Constant product

`ConstantProductCurve` is a Uniswap V2 style pool: two reserves whose product
stays constant across a swap, minus the fee. It reproduces `getAmountsOut` to
the wei:

    inWithFee  = amountIn * feeNum
    amountOut  = inWithFee * reserveOut / (reserveIn * feeDen + inWithFee)

floored, in native units. The fee is given as a numerator and denominator, so
one class covers the forks: Uniswap V2 and SushiSwap are 997/1000 (0.30%),
PancakeSwap V2 is 9975/10000 (0.25%).

Two consequences matter for a backtest. A larger swap gets a worse average rate,
because it moves the reserves further. And the realized rate is below the
marginal rate by an amount that grows with size: that gap is the price impact,
and ignoring it makes a DEX strategy look more profitable than it is.

## Stableswap

`StableSwapCurve` is a Curve stable pool (a 3pool of stablecoins), exact in
integer. It blends constant-sum, for a flat one-for-one price near the peg, with
constant-product, so the pool never empties, tuned by an amplification `A`. The
balances are first normalized to a common scale by per-coin rates (3pool keeps
DAI at 1e18 and lifts USDC and USDT by 1e12), and the invariant `D` and the
output balance both come from integer Newton that stops when the step is within
one unit, exactly as the contract does. The fee is taken on the output after the
contract's defensive `-1`. `A * N` sets the amplification, the original
StableSwap convention.

One class covers 3pool and other plain stableswap pools: it is parameterized by
the balances, the rates, `A`, and the fee, and `n` is the number of coins.

## Cryptoswap

`CryptoswapCurve` is a Curve V2 pool (a tricrypto pool of volatile assets), exact
in integer. It is a direct transcription of the contract's integer algorithm: the
invariant `D` and the output balance come from the contract's Newton solvers, and
the divisions floor exactly where the Vyper floors, so the rounding matches and
not just the formula. The balances are normalized by per-coin precisions and an
internal price scale, the amplification `A` and `gamma` come in their on-chain
packing, and the fee is dynamic, taken on the output and computed from the
post-swap balances so a lopsided pool charges more.

This curve is the pricing surface, and it reproduces the live tricrypto2 get_dy
to the wei. The price scale is a parameter held across a swap here; the internal
repegging that moves the scale over time is a separate piece.

## Cryptoswap repegging

`RepeggingCryptoswapPool` is a `CryptoswapCurve` whose price scale moves. On chain
a Curve V2 pool re-centers its liquidity on the traded price, with no external
oracle, when doing so pays for itself out of accumulated fees. `applySwap` runs
the contract's `tweak_price` after the trade: it advances an EMA price oracle
(through Balancer-style `halfpow`), updates the running fee profit and virtual
price, and steps the price scale toward the oracle when the pool is far enough
ahead, keeping the step only if it leaves the pool in profit. All in the
contract's integer arithmetic, so the evolved scale, oracle, and profit match the
chain. The pricing within a single swap is the exact `CryptoswapCurve`; this adds
the state evolution across swaps.

Each swap advances an internal clock by `dtPerSwap`, since the curve interface
carries no wall-clock time; a backtest sets it to the spacing it wants between
trades.

## Weighted

`WeightedCurve` is a Balancer weighted pool of n assets, exact in integer. The
output for swapping token i into token j is `balanceOut · (1 −
(balanceIn/(balanceIn+amountInAfterFee))^(weightIn/weightOut))`, and the power
goes through Balancer's own fixed-point `pow`, which is `exp(y · ln(x))` with
signed fixed-point `ln` and `exp`. That signed math runs on `i256`, and the
divisions truncate toward zero the way Solidity's `int256` does, so the rounding
matches the contract. Equal weights reduce to constant-product, and the common
integer exponents (1, 2, 4) take Balancer's fast paths without the transcendental.

`WeightedCurve` holds n balances, per-token scaling factors that carry native
amounts into the 1e18 space the math works in, the normalized weights, and the
swap fee.

## Concentrated liquidity

`ConcentratedLiquidityCurve` is a Uniswap v3 style pool, exact in integer. The
swap math is a transcription of the v3 SwapMath, SqrtPriceMath, and FullMath, so
the Q64.96 sqrt-price steps and their up and down rounding match the contract. A
swap walks the initialized ticks: within a range it is a single step on the
active liquidity, and crossing an initialized tick changes the liquidity by that
tick's liquidityNet. The state is the current sqrt price, the active liquidity,
the fee, and the tick table; a large swap can cross several ticks, each on a
different liquidity. It reproduces a v3 pool's QuoterV2 quote to the wei.

## The connector boundary

`AmmDexConnector` presents one token pair of a pool as an order book the rest of
the engine understands. It is the single place where native-wei u256 becomes the
engine's `Quantity` and `Price`, using the two tokens' decimals. The connector
prices its synthetic levels from the curve's `amountOut`, so the book reflects
the real fill at depth. Where a backtest sources its pool state is the concern of
the connector that drives the venue, not of the curve.

## What it does not touch

The CLOB SimulatedExecutor is unchanged. A centralized-exchange backtest fills
against the order book; only an AMM venue fills through a curve. The core engine
stays on its int64 `Decimal` for orders, the book, positions, tapes, and
bindings; the u256 curve math lives only in the curve layer and its native-wei
boundary.
