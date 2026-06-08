# Backtest an LP position

A pool-state tape is a delta log: a list of swaps, optionally with checkpoints. Replay
it through the exact curve and you get a table over time — price, reserves, LP value,
impermanent loss — that a quant can hand straight to pandas. The curve does the wei-exact
math; `flox_py.dex.Tape` carries the decimals and turns the low-level pool tape into a
backtest.

```python
from flox_py import dex

weth = dex.Token("WETH", 18)
usdc = dex.Token("USDC", 6)
pool = dex.UniswapV2(weth, usdc, reserves=("1000 WETH", "2_000_000 USDC"), fee="0.30%")
```

## Build and replay a tape

Add swaps as `(ts, amount, into=None)` — same human `"NUMBER SYMBOL"` form as `quote`.
`replay` runs them through a clone of the pool (the source pool is untouched) and returns
a `DataFrame`, or a list of dicts when pandas is not installed.

```python
tape = dex.Tape(pool).from_swaps([
    (1, "50 WETH"),
    (2, "50 WETH"),
    (3, "100000 USDC"),
])
bt = tape.replay()
#   ts   price      reserve0              reserve1        lp_value      il         trade
#   1    1900.95    1050000000000000...   1900473...      ...           -0.0006    True
#   ...
bt.attrs["drift_count"]        # checkpoints that disagreed with the replayed state
```

Each swap row carries the realized `price`, the post-swap `reserve0` / `reserve1` in wei,
the `lp_value` of the whole pool in token1, and `il` — the pool's value against holding
the starting token mix at that step's price.

## From decoded chain logs

`from_evm_logs` takes decoded Uniswap v2 `Swap` logs (the `eth_getLogs` dicts, data word
order `amount0In, amount1In, amount0Out, amount1Out`) and builds the tape directly. The
`pool`'s `token0` / `token1` must match the on-chain order.

```python
tape = dex.Tape.from_evm_logs(pool, swap_logs)   # swap_logs from your RPC provider
bt = tape.replay()
```

## Impermanent loss on a position

`LpPosition` pins to its entry. Snapshot it against a pool, move the pool, and read the
loss against HODL of the entry mix. The value is share-scaled, so it works for a stake of
any size; pass `value` as a token1 amount.

```python
pos = dex.LpPosition(pool, value="100000 USDC")   # a 100k-USDC stake
pos.impermanent_loss()           # Decimal('0') -- nothing has moved yet

pool.swap("200 WETH")            # price drops
pos.value()                      # current worth in USDC
pos.hodl_value()                 # what holding the entry mix is worth now
pos.impermanent_loss()           # < 0 -- the LP underperforms holding
```

For a fee-free constant-product pool the loss matches the closed form
`2 * sqrt(r) / (1 + r) - 1`, where `r` is the price ratio since entry.

## Checkpoints and drift

A checkpoint asserts the pool's reserves at a timestamp. If the replayed state disagrees —
a missed swap, a wrong fee, a reordered tape — the row is flagged and counted, so a tape
that does not reconstruct the on-chain pool is caught rather than silently believed.

```python
tape.checkpoint(2, "999 WETH", "2_000_000 USDC")
bt = tape.replay()
bt.attrs["drift_count"]          # 1 if the checkpoint did not match
```
