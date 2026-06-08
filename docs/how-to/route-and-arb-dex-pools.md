# Route, arb, and ingest DEX pools

`flox_py.dex` prices one pool. The desk tools work across pools: route a notional to the
venue that fills it best, size the arb between two pools, and read a live pool (or its
swap history) straight off the chain into the comfort layer. Everything stays wei-exact —
routing and arb price through `quote`, which leaves the pools untouched.

```python
from flox_py import dex

weth, usdc = dex.Token("WETH", 18), dex.Token("USDC", 6)
uni = dex.UniswapV2(weth, usdc, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
ray = dex.RaydiumCp(weth, usdc, reserves=("1000 WETH", "2_000_000 USDC"), trade_fee="0.25%")
```

## Best execution

`Router.best` returns the `(pool, Quote)` that fills the most. `table` ranks every venue.

```python
r = dex.Router([uni, ray])
pool, q = r.best("50000 USDC", into="WETH")    # the venue that returns the most WETH
r.table("50000 USDC", into="WETH")
#   venue       out_human   impact_pct
#   RaydiumCp   24.3308     2.677
#   UniswapV2   24.3189     2.725
```

A pool that cannot price the swap (an unknown token, a vanishing output) is skipped, so a
mixed router still routes through the venues that can fill.

## Size an arb

`arb` finds the input size that maximises the spread between two pools on the same pair. It
spends token1 on the cheaper pool to buy token0, sells that token0 on the dearer pool, and
returns the size and the token1 profit. Profit is concave in size, so the optimum is
interior; the figure matches running the two legs for real.

```python
cheap = dex.UniswapV2(weth, usdc, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
dear  = dex.UniswapV2(weth, usdc, reserves=("1000 WETH", "2_100_000 USDC"), fee="0.30%")

a = dex.arb(cheap, dear)
a.size            # 21717.774391 USDC   -- the profit-maximising input
a.profit          # 469.578256 USDC     -- net, after both pools' fees and slippage
a.route           # ('UniswapV2', 'UniswapV2')  buy on cheap, sell on dear
a.profitable      # True
```

When the two pools agree on price there is no edge and `route` is `None`.

## From a chain address

The ingest adapters read state over a JSON-RPC endpoint, injected as a plain callable
`rpc(method, params)` — a live provider, a cached fixture, or a test double all fit the
same shape.

```python
pool = dex.from_evm_pool("0x88e6...5640", usdc, weth, rpc, kind="v3", fee="0.05%")
pool.spot_price       # read from slot0() + liquidity()

# v2 reads getReserves(); pass the tokens in on-chain order (they carry the decimals).
v2 = dex.from_evm_pool("0xB4e1...c0Dc", weth, usdc, rpc, kind="v2", fee="0.30%")
```

Turn a block range of swaps into a backtest in one call:

```python
tape = dex.tape_from_evm_logs(pool, rpc, from_block=19_000_000, to_block=19_000_500,
                              address="0xB4e1...c0Dc")
bt = tape.replay()    # the price / reserves / IL table from "Backtest an LP position"
```

On Solana, `tape_from_solana` reads each transaction's vault balance deltas — the amount
in and out and the direction come straight from the pre/post token balances, so it is
venue-agnostic and exact.

```python
tape = dex.tape_from_solana(pool, signatures, rpc, vault0=base_vault, vault1=quote_vault)
```
