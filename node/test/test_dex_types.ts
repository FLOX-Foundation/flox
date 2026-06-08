// Type-level test for flox/dex. `tsc --noEmit` checks lib/dex.d.ts compiles and that
// the documented usage type-checks. No runtime; behaviour is in test/test_dex.js.

import {
  Token, Amount, Pool, UniswapV2, RaydiumCp, UniswapV3,
  Tape, LpPosition, Router, Arb, arb,
} from '../lib/dex';

const weth = new Token('WETH', 18);
const usdc = new Token('USDC', 6);

const wei: bigint = weth.toWei('1.5');
const human: string = weth.toHuman(wei);
const amt: Amount = usdc.amount('50000');
const parsed: Amount = Amount.parse('10 WETH', { WETH: weth, USDC: usdc });

const uni: UniswapV2 = new UniswapV2(weth, usdc, { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });
const ray: RaydiumCp = new RaydiumCp(weth, usdc, { reserves: ['1000 WETH', '2000000 USDC'], tradeFee: '0.25%' });
const v3: UniswapV3 = new UniswapV3(usdc, weth, {
  sqrtPriceX96: 1959100328691929984878240664321702n, liquidity: 2580696918646962643n, fee: '0.05%',
});
const v3b: UniswapV3 = UniswapV3.fromPrice(usdc, weth, { price: 2000, liquidity: 1n, fee: '0.05%' });

const q = uni.quote('10 WETH', 'USDC');
const exact: bigint = q.exactWei;
const impact: number = q.priceImpact;
const spot: number = uni.spotPrice;
const reserves: [Amount, Amount] = uni.reserves;
const sqrt: bigint | null = v3.sqrtPrice;
const cloned: UniswapV2 = uni.clone();

const router = new Router([uni, ray]);
const best: [Pool, any] = router.best('50000 USDC', 'WETH');
const a: Arb = arb(uni, ray);
const profitable: boolean = a.profitable;

const tape = new Tape(uni).fromSwaps([[1, '50 WETH'], [2, '50 WETH', 'USDC']]);
const rows = tape.replay();
const drift: number = rows.driftCount;
const r0: bigint = rows[0].reserve0;

const pos = new LpPosition(uni, '100000 USDC');
const il: number = pos.impermanentLoss();

// Reference the bindings so noUnusedLocals (if ever enabled) stays quiet and the
// imports are all exercised.
void [wei, human, amt, parsed, v3b, exact, impact, spot, reserves, sqrt, cloned,
      best, profitable, drift, r0, il];
