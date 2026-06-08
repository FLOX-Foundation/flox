# Price and backtest DEX pools on Node

`flox/dex` is the Node mirror of the Python `flox_py.dex` comfort layer: the same object
model — `Token`, `Amount`, `Pool` venue classes, `Quote`, `Router`, `arb`, `Tape` — over
the same exact AMM curves. Human amounts go in as `"NUMBER SYMBOL"` strings, swap math
runs in the native addon as `bigint` wei, and a mis-routed swap raises instead of
returning a quiet zero. The figures match the Python layer to the wei.

It ships as the `dex` property of the package (the raw `ammCurve*` / `poolTape*`
functions stay on the top level for power users).

```js
const { dex } = require('@flox-foundation/flox');

const weth = new dex.Token('WETH', 18);
const usdc = new dex.Token('USDC', 6);

const pool = new dex.UniswapV2(weth, usdc,
  { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });

pool.spotPrice;                       // 2000  -- token1 per token0, fee excluded
pool.quote('10 WETH').out.toString(); // '19743.160687 USDC'
pool.quote('10 WETH').exactWei;       // 19743160687n  -- the raw curve output
```

`quote` leaves the pool where it is; `swap` moves it; `clone` copies it so you can size a
trade without disturbing the live one.

## Concentrated liquidity

A Uniswap v3 pool is built from its on-chain `sqrtPriceX96` and liquidity, or from a human
price with `fromPrice`. Its state reads back.

```js
const v3 = new dex.UniswapV3(usdc, weth, {
  sqrtPriceX96: 1959100328691929984878240664321702n,
  liquidity: 2580696918646962643n,
  fee: '0.05%',
});
v3.quote('1000 USDC').out.toString(); // '0.61112890703349149 WETH'
v3.sqrtPrice;                         // 1959100328691929984878240664321702n
```

## Route and arb

`Router.best` returns the venue that fills the most; `arb` sizes the depth-aware spread
between two pools on the same pair, validated against running both legs for real.

```js
const ray = new dex.RaydiumCp(weth, usdc,
  { reserves: ['1000 WETH', '2000000 USDC'], tradeFee: '0.25%' });

const [venue, q] = new dex.Router([pool, ray]).best('50000 USDC', 'WETH');
venue.venue;            // 'RaydiumCp'  -- the lower fee fills more

const cheap = new dex.UniswapV2(weth, usdc, { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });
const dear  = new dex.UniswapV2(weth, usdc, { reserves: ['1000 WETH', '2100000 USDC'], fee: '0.30%' });
const a = dex.arb(cheap, dear);
a.size.toString();      // '21717.774391 USDC'  -- the profit-maximising input
a.profit.toString();    // '469.578256 USDC'    -- net of both pools' fees and slippage
a.route;                // ['UniswapV2', 'UniswapV2']
```

## Backtest a tape

A `Tape` replays swaps through the exact curve into typed rows — price, reserves (`bigint`
wei), LP value, impermanent loss, drift. The result is a plain array (no DataFrame
dependency); hand it to any charting lib.

```js
const tape = new dex.Tape(pool).fromSwaps([
  [1, '50 WETH'],
  [2, '50 WETH'],
  [3, '100000 USDC'],
]);
const rows = tape.replay();
rows[rows.length - 1].reserve0;   // bigint, exact
rows.driftCount;                  // checkpoints that disagreed with the replay

const pos = new dex.LpPosition(pool, '100000 USDC');
pos.impermanentLoss();            // 0 at entry, negative once the price moves
```

For the Python equivalent of every call here, see [Price a DEX swap](price-a-dex-swap.md)
and [Backtest an LP position](backtest-an-lp-position.md).
