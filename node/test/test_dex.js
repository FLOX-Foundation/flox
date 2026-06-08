// test_dex.js -- the flox/dex comfort layer (W24-T005), the Node mirror of
// python/tests/test_dex*.py. Verifies every quote matches the raw ammCurve* binding to
// the wei, that the silent-wrong-answer footgun raises, that CLMM state reads back, and
// that the router / arb / tape produce the same figures as the Python suite.

'use strict';

const flox = require('..');
const dex = flox.dex;

let failed = false;
function check(label, ok) {
  if (!ok) { console.error('FAIL: ' + label); failed = true; }
  else console.log('ok: ' + label);
}
function throws(label, fn) {
  let threw = false;
  try { fn(); } catch (_) { threw = true; }
  check(label, threw);
}

const WETH = new dex.Token('WETH', 18);
const USDC = new dex.Token('USDC', 6);

// -- units --
(function testAmountAndToken() {
  const a = dex.Amount.parse('50000 USDC', { USDC, WETH });
  check('amount parse wei', a.wei === 50000n * 10n ** 6n && a.token === USDC);
  check('toWei fraction', WETH.toWei('1.5') === 15n * 10n ** 17n);
  check('amount toString', WETH.amount('2.5').toString() === '2.5 WETH');
  check('toHuman exact', USDC.toHuman(50000n * 10n ** 6n) === '50000');
})();

// -- constant product, exact vs raw curve --
(function testConstantProduct() {
  const p = new dex.UniswapV2(WETH, USDC, { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });
  check('spot price', p.spotPrice === 2000);
  const q = p.quote('10 WETH');
  const raw = flox.ammCurveConstantProduct(1000n * 10n ** 18n, 2000000n * 10n ** 6n, 997, 1000);
  check('quote matches raw to the wei',
    q.exactWei === flox.ammCurveAmountOut(raw, 0, 1, 10n * 10n ** 18n));
  check('quote out token + impact', q.out.token === USDC && q.priceImpact > 0);
  // PancakeSwap 0.25%.
  const pc = new dex.UniswapV2(WETH, USDC,
    { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.25%', feeDen: 10000 });
  const rawPc = flox.ammCurveConstantProduct(1000n * 10n ** 18n, 2000000n * 10n ** 6n, 9975, 10000);
  check('pancake fee maps',
    pc.quote('10 WETH').exactWei === flox.ammCurveAmountOut(rawPc, 0, 1, 10n * 10n ** 18n));
})();

// -- uniswap v3 read-back + from_price --
(function testV3() {
  const v3 = new dex.UniswapV3(USDC, WETH,
    { sqrtPriceX96: 1959100328691929984878240664321702n, liquidity: 2580696918646962643n, fee: '0.05%' });
  check('v3 quote to the wei', v3.quote('1000 USDC').exactWei === 611128907033491490n);
  check('v3 sqrt read-back', v3.sqrtPrice === 1959100328691929984878240664321702n);
  check('v3 liquidity read-back', v3.liquidity === 2580696918646962643n);
  const built = dex.UniswapV3.fromPrice(USDC, WETH,
    { price: v3.spotPrice, liquidity: 2580696918646962643n, fee: '0.05%' });
  const rel = Number(built.sqrtPrice - v3.sqrtPrice) / Number(v3.sqrtPrice);
  check('fromPrice reconstructs sqrt', Math.abs(rel) < 1e-9);
})();

// -- guard-rail kills the footgun --
(function testGuardrail() {
  const v3 = new dex.UniswapV3(USDC, WETH,
    { sqrtPriceX96: 1959100328691929984878240664321702n, liquidity: 2580696918646962643n, fee: '0.05%' });
  throws('unknown token raises', () => v3.quote('1 DAI'));
  throws('vanishing output raises', () => v3.quote('0.000000000001 WETH'));
})();

// -- clone + depth monotone --
(function testCloneDepth() {
  const p = new dex.UniswapV2(WETH, USDC, { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });
  const c = p.clone();
  p.swap('100 WETH');
  check('clone untouched', c.reserves[0].wei === 1000n * 10n ** 18n);
  check('original moved', p.reserves[0].wei === 1100n * 10n ** 18n);
  const rows = c.depth(['1 WETH', '50 WETH', '200 WETH']);
  const impacts = rows.map((r) => r.impactPct);
  check('slippage monotone',
    impacts[0] > 0 && impacts[0] <= impacts[1] && impacts[1] <= impacts[2]);
})();

// -- router best matches direct quote --
(function testRouter() {
  const uni = new dex.UniswapV2(WETH, USDC, { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });
  const ray = new dex.RaydiumCp(WETH, USDC, { reserves: ['1000 WETH', '2000000 USDC'], tradeFee: '0.25%' });
  const r = new dex.Router([uni, ray]);
  const [pool, q] = r.best('50000 USDC', 'WETH');
  check('router picks lower-fee venue', pool === ray);
  check('router quote is that pool\'s own', q.out.wei === ray.quote('50000 USDC', 'WETH').out.wei);
  const tbl = r.table('50000 USDC', 'WETH');
  check('table sorted best-first', tbl[0].venue === 'RaydiumCp' && tbl[0].outHuman >= tbl[1].outHuman);
})();

// -- arb depth-aware, validated by clone execution --
(function testArb() {
  const cheap = new dex.UniswapV2(WETH, USDC, { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });
  const dear = new dex.UniswapV2(WETH, USDC, { reserves: ['1000 WETH', '2100000 USDC'], fee: '0.30%' });
  const a = dex.arb(cheap, dear);
  check('arb profitable', a.profitable && a.profit.wei > 0n);
  check('arb interior', a.size.wei > 0n && a.size.wei < dear.reserves[1].wei);
  // Re-run on clones and confirm the realised profit is exactly a.profit.
  const buy = cheap.clone(), sell = dear.clone();
  const t0out = buy.swap(USDC.amount(USDC.toHuman(a.size.wei)), 'WETH').out;
  const t1back = sell.swap(WETH.amount(t0out.humanString), 'USDC').out;
  check('arb profit matches clone execution', t1back.wei - a.size.wei === a.profit.wei);
  const same = dex.arb(cheap.clone(), cheap.clone());
  check('no edge when equal', !same.profitable && same.route === null);
})();

// -- tape replay matches the raw curve to the wei --
(function testTape() {
  const pool = new dex.UniswapV2(WETH, USDC, { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });
  const tape = new dex.Tape(pool).fromSwaps([[1, '50 WETH'], [2, '50 WETH'], [3, '100000 USDC']]);
  const rows = tape.replay();
  const c = flox.ammCurveConstantProduct(1000n * 10n ** 18n, 2000000n * 10n ** 6n, 997, 1000);
  flox.ammCurveApplySwap(c, 0, 1, 50n * 10n ** 18n);
  flox.ammCurveApplySwap(c, 0, 1, 50n * 10n ** 18n);
  flox.ammCurveApplySwap(c, 1, 0, 100000n * 10n ** 6n);
  check('tape final reserve0', rows[rows.length - 1].reserve0 === flox.ammCurveBalance(c, 0));
  check('tape final reserve1', rows[rows.length - 1].reserve1 === flox.ammCurveBalance(c, 1));
  check('source pool untouched', pool.reserves[0].wei === 1000n * 10n ** 18n);
})();

// -- IL zero at entry, negative after a move; checkpoint drift flagged --
(function testIlAndDrift() {
  const pool = new dex.UniswapV2(WETH, USDC, { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });
  const pos = new dex.LpPosition(pool, '100000 USDC');
  check('IL zero at entry', pos.impermanentLoss() === 0);
  pool.swap('200 WETH');
  check('IL negative after move', pos.impermanentLoss() < 0);

  const t = new dex.Tape(pool.clone());
  t.swap(1, '50 WETH');
  t.checkpoint(2, '999 WETH', '2000000 USDC');
  const rows = t.replay();
  check('checkpoint drift flagged', rows.driftCount === 1);
})();

if (failed) { console.error('test_dex: FAIL'); process.exit(1); }
console.log('test_dex: OK');
