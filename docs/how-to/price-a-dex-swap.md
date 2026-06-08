# Price a DEX swap

`flox_py.dex` prices a swap against an AMM pool, exact to the wei, with a surface a
quant can read: human amounts in and out, swap directions by token symbol, and pools
that introspect. The exact integer curve does the math underneath; this layer carries
the decimals and the token order so a mis-routed swap raises instead of returning a
quiet zero.

```python
from flox_py import dex

weth = dex.Token("WETH", 18)
usdc = dex.Token("USDC", 6)

pool = dex.UniswapV2(weth, usdc, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
pool                     # <UniswapV2 WETH/USDC  px=2000.00  reserves=(1000 WETH, 2000000 USDC)>
pool.spot_price          # Decimal('2000') -- token1 per token0, fee excluded
```

## Quote a swap

`quote` takes a human amount as `"NUMBER SYMBOL"` and infers the direction from the
token. It returns a `Quote` with the output amount, the price impact, the realized
average price, and the exact wei.

```python
q = pool.quote("10 WETH")
q.out             # 19743.160687 USDC
q.price_impact    # Decimal('0.01284...')   ~1.28%
q.avg_price       # Decimal('1974.31...')
q.exact_wei       # 19743160687   -- the raw curve output, lossless
```

`quote` does not move the pool. `swap` does, and returns the same `Quote`; `clone`
copies the pool so you can size a trade without disturbing the live one.

```python
sized = pool.clone()
sized.swap("100 WETH")           # moves `sized`
pool.reserves                    # unchanged
```

## Concentrated liquidity

A Uniswap v3 pool is built from its on-chain `sqrtPriceX96` and liquidity, or from a
human price with `from_price`. Its state reads back.

```python
v3 = dex.UniswapV3(usdc, weth, 1959100328691929984878240664321702,
                   2580696918646962643, fee="0.05%")
v3.quote("1000 USDC").out        # 0.611128907033491490 WETH
v3.sqrt_price, v3.liquidity      # the current state, exact

# Or build from a price instead of a raw Q64.96 number:
v3b = dex.UniswapV3.from_price(usdc, weth, price=v3.spot_price,
                               liquidity=2580696918646962643, fee="0.05%")
```

## A slippage table

`depth` prices a list of sizes and returns the realized price and impact per size — a
pandas `DataFrame` when pandas is installed, a list of dicts otherwise.

```python
pool.depth(["1 WETH", "10 WETH", "50 WETH", "200 WETH"])
#   in        out            avg_price   impact_pct
#   1 WETH    1992.01 USDC   1992.01     0.100
#   10 WETH   19743.16 USDC  1974.32     0.987
#   ...
```

## Guard-rails

The raw `AmmCurve` prices by token index, so a wrong token order or decimals returns a
silent near-zero. The comfort layer owns the decimals and raises instead:

```python
pool.quote("1 DAI")               # ValueError: unknown token 'DAI'
pool.quote("0.000000000001 WETH") # ValueError: output 0 -- check token order / decimals
```

## Venues

`UniswapV2` (and forks via `fee` / `fee_den`), `RaydiumCp` (Solana, `trade_fee` over a
1e6 denominator), and `UniswapV3` share the same `quote` / `spot_price` / `reserves` /
`clone` surface. The fee string is per venue: `"0.30%"` is a Uniswap v2 0.30% pool,
`"0.25%"` a Raydium 0.25% pool.
