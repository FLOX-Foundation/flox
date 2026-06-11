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

A swap can be exact-input (spend a fixed input) or exact-output (take a fixed
output), and the contract rounds the price the opposite way for each.
`amountInForOutput` / `applySwapForOutput` are the exact-output side: the input
needed to take a target output. Both branches are checked against live pool Swap
logs -- the log carries the post sqrtPriceX96, so the curve replaying the swap must
reproduce it -- and each reproduces its swaps to the wei.

## Solana: Raydium constant product

The exact-curve approach is not EVM-specific. `RaydiumCpCurve` is a Raydium
constant-product pool on Solana, transcribed from its program
(`CurveCalculator::swap_base_input`). The core is the same constant product as the
EVM forks, `out = net * outVault / (inVault + net)` floored, but the fee handling
is Raydium's, over a 1e6 denominator as a ceil-div. The trade fee comes off the
input. The creator fee has two on-chain modes: on input, the trade and creator
rates are summed and removed from the input in one ceil-div; on output, only the
trade fee comes off the input and a ceil-div creator fee then comes off the
swapped amount, so the user receives less than the pool releases. A pool with no
creator fee, the common case, is identical either way.

The two balances are the swappable reserves -- each vault's balance minus its
accumulated protocol, fund, and creator fees, which is what the program feeds the
curve. They move the way the program's result does: the input reserve grows by the
input net of fees (the fee is set aside, not added to the reserve), and the output
reserve falls by the full swapped amount. It is a separate class from
`ConstantProductCurve`, sharing the formula but not the fee, and it implements the
same `INTokenCurve` -- a Solana pool is just another curve, no interface change. It
reproduces the program's own integer test vectors to the lamport.

Solana state lives in account data rather than contract getters, and the programs
have no quote view to ask, so a Solana curve is checked against the program's
published test vectors and the protocol's off-chain SDK rather than a single
on-chain call. The concentrated-liquidity Solana pools (Orca, Raydium) carry
their own fixed-point and are separate transcriptions.

## Solana: Orca Whirlpool concentrated liquidity

`OrcaWhirlpoolCurve` is an Orca Whirlpool, Solana's concentrated-liquidity pool.
It is the Uniswap v3 swap math at a Q64.64 sqrt price instead of Q64.96, so it is
a thin parameterization of `ConcentratedLiquidityCurve`, not a separate
transcription: same delta and next-sqrt-price formulas, same tick walk, only the
fixed-point unit and the min/max sqrt price change. The Whirlpool program writes
`get_amount_delta_a` as one division over the full 256-bit numerator where v3
nests two divisions, but those give the same result for every input
(`ceil(ceil(a/b)/c) == ceil(a/(b·c))`, and the floor analogue), so the rounding is
identical, not merely close.

The state is the current Q64.64 sqrt price, the active liquidity, the fee rate in
hundredths of a basis point, and the table of initialized ticks. A swap within a
range is one step on the active liquidity; a larger swap crosses ticks, each on a
different liquidity, exactly as on chain. This models a static-fee Whirlpool, the
common kind; the opt-in adaptive-fee extension (the program's volatility-tracking
FeeRateManager) is a separate feature, not part of this curve, the same way the v3
curve does not model v4 hooks.

Validation is two-sided: the curve reproduces a faithful transcription of the
program's swap to the unit on no-cross and tick-crossing cases, and a live
Whirlpool read (the pool account plus its tick arrays, with each tick index
converted to its sqrt price by the program's tick math) priced through the curve
agrees with an independent Jupiter quote to a fraction of a basis point, the
residual being the aggregator's cache lag, not the math.

## Solana: Whirlpool adaptive fee

A Whirlpool can opt into an adaptive fee, and `AdaptiveFeeWhirlpoolCurve` is the
`OrcaWhirlpoolCurve` swap with it. The fee is no longer fixed: it rises with how far
the price has moved across tick groups since a time-decayed reference, so the swap
is priced per step. Each step is bounded to the next tick-group boundary so the fee
is constant within it; a volatility accumulator updates as groups are crossed, and
the step fee is the static fee plus an adaptive term quadratic in that accumulator.
This is a transcription of the program's FeeRateManager and Oracle, minus the skip
optimization (which bounds compute, not output, so omitting it is exact). The
current tick and the swap timestamp come from the pool; the fee state -- the
volatility accumulator and its reference, plus the constants -- comes from the
pool's Oracle account.

It is validated to the unit against the program's own pre-calculated fee-rate
vector (the volatility-to-fee table), and the tick-group-bounded swap reduces
exactly to the static `OrcaWhirlpoolCurve` when the adaptive control factor is zero
(the program skips the bounding there), so the loop is anchored to a curve already
checked against the chain. The full adaptive swap matches a faithful transcription
of the program. Unlike the static curves, a live cross-check is weaker here: the
volatility accumulator is time-dependent, so an aggregator's lagged state cannot be
aligned -- the program's own vectors are the stronger gate.

## Solana: Raydium CLMM

`RaydiumClmmCurve` is Raydium's concentrated-liquidity pool, the other large
Solana CLMM. It is the same `ConcentratedLiquidityCurve` at Q64.64, a thin
parameterization like the Whirlpool: Raydium's `get_delta_amount_0_unsigned`
already uses the nested rounding the v3 core does, and its next-sqrt-price and 1e6
fee match, so only the fixed-point unit and the maximum sqrt price differ from v3
(and the maximum differs slightly from Orca's, because Raydium's tick math rounds
the boundary differently). The standard fee-on-input pool is modelled; the
fee-on-output path for transfer-fee mints and Token-2022 transfer fees are a
separate boundary concern, not part of the curve.

Validated to the unit against a faithful transcription of the program's swap on
no-cross and tick-crossing cases. The live read differs from the Whirlpool only in
the account layouts and the tick math constants (Raydium uses the Uniswap-style
multiplicative tick table); the curve and its tick walk are the shared core.

## Solana: Meteora DLMM (discrete bins)

`MeteoraDlmmCurve` is a Meteora DLMM (Liquidity Book) pool, and it is not a curve in
the AMM sense: liquidity sits in discrete price bins, each a constant-sum segment at
its own fixed price `(1 + bin_step/1e4)^id` in Q64.64, and a swap walks bins from the
active one outward. The price is the program's Liquidity Book power -- a bit-
decomposition with an inversion trick, an approximation of the real, so the bin
price is the chain's value, not `1.001^id` exactly. Each bin fills constant-sum:
output is `price * amount` or `amount / price` shifted by the Q64 scale.

The fee is dynamic, like the Whirlpool adaptive fee: a base fee plus a variable fee
quadratic in a volatility accumulator that grows as the swap crosses bins from a
time-decayed reference. It is priced per bin -- the accumulator updates at each bin,
and the fee comes off the input. This is a transcription of the program's quote for
the dominant case: MM liquidity, exact-in, fee on input. Limit orders (a newer
layer in each bin) and the fee-on-output mode are separate features, not modelled.

Validated to the unit against a faithful transcription of the program's swap, and
the bin prices match the program's Liquidity Book power exactly. As with the
adaptive fee, a clean live cross-check is not the gate: the variable fee depends on
the pool's volatility state, which is time-dependent, so a lagged aggregator cannot
be aligned.

## Solana: Saber StableSwap

Saber is the Curve StableSwap on Solana, and it needs no new curve: it is a
`StableSwapCurve`. The invariant is the same, the amplification is the same
`Ann = A * N` convention (Saber's `ann = amp * N_COINS`), the output Newton and the
defensive `dy = dest - y - 1` match, and a Saber pool is two coins with no rate
scaling -- so it is a `StableSwapCurve` with identity rates and `n = 2`. Saber's
fee is a numerator over a denominator rather than a fixed 1e10 scale; when the
denominator divides 1e10, as Saber's powers of ten do, it maps onto the over-1e10
fee exactly. So Saber pools are priced by the existing curve, validated to the
unit against a transcription of Saber's `swap_to`, and read in the harness from
Saber's packed (pre-Anchor) SwapInfo account and its two reserve vaults.

## Token-2022 transfer fees

A Solana mint can carry a transfer fee (the Token-2022 TransferFeeConfig
extension), withheld on every transfer of that token. It changes the fill a swap
delivers even when the pool curve is exact: the pool receives the input net of its
transfer fee, and the user receives the output net of its transfer fee.
`Token2022TransferFee` is that fee, exact per the program -- ceil(amount * basis
points / 10000), capped at a maximum, with the rate the mint has in effect for the
current epoch. `amountOutWithTransferFees` composes it with any curve: feed the
pool the input net of its fee, take the curve output, and subtract the output
token's fee. It is a boundary concern, parameterized by the mint's config, not a
property of the curve, so it composes once and applies to every venue.

## The connector boundary

`AmmDexConnector` presents one token pair of a pool as an order book the rest of
the engine understands. It is the single place where native-wei u256 becomes the
engine's `Quantity` and `Price`, using the two tokens' decimals. The connector
prices its synthetic levels from the curve's `amountOut`, so the book reflects
the real fill at depth. Where a backtest sources its pool state is the concern of
the connector that drives the venue, not of the curve.

The synthetic book is convenient for a strategy that reuses CEX book logic, but a
fixed set of levels discretises away the curve's exactness -- the very thing the
exact curves are for. So the connector also exposes the curve directly, as
`IPoolQuoteView`: `quoteOut(amountIn, baseForQuote)` is the exact output for an
arbitrary size with no book rounding, `reserves()` is the pool's exact composition
(the virtual reserves for a concentrated pool), and `curve()` reaches venue-specific
state a generic view cannot name -- a concentrated pool's sqrt price and liquidity,
a stable pool's amplification. A DEX-native strategy (LP / MM accounting,
impermanent loss, an exact-size taker swap) holds the view as a plain interface,
never having to know the venue is a pool, and it always reflects the live curve as
the replay applies swaps and checkpoints. The book stays as one optional view layered
on top, not the only interface.

## Replaying a pool over time

A curve prices one state of a pool; a backtest needs the pool's state as it moved.
`AmmPoolReplaySource` replays a recorded sequence of pool-state snapshots -- each a
ready-to-price curve at a timestamp -- through an `AmmDexConnector`: at each
snapshot it points the connector at that curve and republishes the synthetic book,
stamped at the snapshot's time. Because a snapshot is just a curve, it is
venue-agnostic -- the same driver replays a constant-product pool, a Whirlpool, or
a Saber pool -- and it is reproducible, since the same snapshots replay
identically. Where the snapshots come from (a recorded tape, a live account read)
is the concern of whatever captured them; this is the engine-side glue that turns
a pool's state-over-time into the book stream the rest of a backtest consumes.

## The pool-state tape

A pool-state tape records a pool's history as a delta log, not snapshots: a
Descriptor (the venue and its static parameters), periodic Checkpoints (the full
state), and the SwapDeltas that move it. A pool's state is *derived* by replaying
the deltas through the exact curve -- a parsed swap replayed via `applySwap`
reconstructs the post-state to the wei, which is what the exact curves are for -- so
the tape is compact and a swap is both the trade and the state mutation, one stream.
`PoolStateReplay` reads a tape and drives an `AmmDexConnector`: a Checkpoint rebuilds
the curve, a SwapDelta is applied through `onSwap`. All amounts are u256, 32 bytes
big-endian, chain-native.

A Checkpoint is venue-shaped: reserves for a constant-product pool; the sqrt
price, active liquidity, and tick array for a concentrated-liquidity pool (Uniswap
v3, Orca Whirlpool, Raydium CLMM); the full balance vector for an n-token pool
(StableSwap, Weighted -- Cryptoswap also carries its price scale, since the chain
repegs it, so a repeg between checkpoints re-anchors rather than corrupts); and the
bin book plus volatility-accumulator state for Meteora DLMM. The replay rebuilds
the matching curve from whichever the Descriptor names, so a cross-tick swap is
reconstructed through the exact v3 math and a DLMM swap through the exact bin walk,
and the drift check compares the replayed and checkpoint states through
`balances()`, which every venue exposes.

An n-token venue's SwapDelta names its `(i, j)` pair explicitly. On the pair the
connector presents it prints a trade; off it -- a 3pool USDC->USDT swap while the
connector presents DAI/USDC -- the shared state still moves and the book
republishes, because that swap changed the pool the presented pair prices from.

It is checkpoint-anchored, not delta-only. Before a Checkpoint re-anchors, the
replayed state is compared to it, and a mismatch is counted as drift -- an
unobserved or unmodelled mutation (a donation, a rebasing token, an admin parameter
change) is caught, never silently carried. Checkpoint cadence is a knob: the worst
case for any chain is a Checkpoint per event, which always works; deltas are the win
where the mutation is observable and exactly modelled. Where the deltas come from --
EVM event logs, parsed Solana instructions -- is the per-chain ingest; the tape and
replay know nothing about the chain.

The records ride on the same binary-log timeline as trades and books. A pool record
is an `EventType::PoolState` frame: a fixed header (timestamp, symbol, the record
kind and venue) and the u256 payload, written through `BinaryLogWriter::writePoolState`
alongside trades and surfaced by the readers in one timestamp-ordered stream. The
frame is additive -- a reader that predates it skips it by its size -- so a strategy
sees a pool's swaps and an unrelated instrument's trades interleaved in time, and a
`PoolState` event's payload drives `PoolStateReplay::step` to evolve the curve. A
chain ingest is then just "parse an event into a pool record, stamp it, append it."

## Live, with reorgs

A recorded tape is settled history; a live feed is not -- a chain can roll back an
unfinalised block (an EVM reorg, a dropped Solana slot), so a swap is optimistic until
its block finalises. `ReorgSafePoolFeed` keeps a finalised curve (the last
irreversible state) and a buffer of optimistic swaps applied on top. A reorg drops the
optimistic swaps above the rolled-back height and rebuilds the working curve from the
finalised one, so a block that did not stick can never corrupt the state; finalising
folds the now-irreversible swaps into the finalised curve. With no reorg the working
curve is exactly what the tape replay produces from the same deltas, so a strategy
sees an identical book live and from a tape -- the live transport (a poll, an
`eth_subscribe`, a Geyser stream) only changes where the deltas come from.

## What it does not touch

The CLOB SimulatedExecutor is unchanged. A centralized-exchange backtest fills
against the order book; only an AMM venue fills through a curve. The core engine
stays on its int64 `Decimal` for orders, the book, positions, tapes, and
bindings; the u256 curve math lives only in the curve layer and its native-wei
boundary.
