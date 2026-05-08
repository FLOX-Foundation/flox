'use strict';
/**
 * node/test/test_execution_algos.js — smoke-test the C++-backed
 * execution algos NAPI bindings (TWAP / VWAP / Iceberg / POV).
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

console.log('=== TWAP ===');
{
  const algo = new flox.TWAPExecutor({
    targetQty: 100, side: 'buy', symbol: 1,
    durationNs: 10_000, sliceCount: 5, startTimeNs: 0,
  });
  const c1 = algo.step(0);
  check(c1.length === 1, 'TWAP first step emits one slice at start');
  check(Math.abs(c1[0].qty - 20) < 1e-9, 'TWAP slice size = target / count');
  const c2 = algo.step(2_000);
  check(c2.length === 1, 'TWAP next slice fires at next interval');
  const cAll = algo.step(10_000);
  check(algo.isDone(), 'TWAP completes by end of duration');
}

console.log('=== VWAP ===');
{
  const algo = new flox.VWAPExecutor({
    targetQty: 100, side: 'buy', symbol: 1,
    volumeCurve: [[1000, 200], [2000, 300], [3000, 500]],
  });
  const c1 = algo.step(2500);
  check(c1.length === 2, 'VWAP emits one slice per elapsed bar');
  // Total volume = 1000; share for first bar = 200/1000 = 0.2, qty = 20
  check(Math.abs(c1[0].qty - 20) < 1e-9, 'VWAP first slice tracks volume share');
  const c2 = algo.step(3000);
  check(c2.length === 1, 'VWAP last bar fires at boundary');
  check(algo.isDone(), 'VWAP completes after curve exhausted');
}

console.log('=== Iceberg ===');
{
  const algo = new flox.IcebergExecutor({
    targetQty: 50, side: 'buy', symbol: 1,
    type: 'limit', limitPrice: 100, visibleQty: 10,
  });
  const c1 = algo.step(0);
  check(c1.length === 1 && Math.abs(c1[0].qty - 10) < 1e-9, 'Iceberg shows 10');
  const c2 = algo.step(1);
  check(c2.length === 0, 'Iceberg does not refill while child is outstanding');
  algo.reportFill(10);
  const c3 = algo.step(2);
  check(c3.length === 1, 'Iceberg refills once child filled');
}

console.log('=== POV ===');
{
  const algo = new flox.POVExecutor({
    targetQty: 100, side: 'buy', symbol: 1,
    participationRate: 0.10, minSliceQty: 1,
  });
  const c1 = algo.step(0);
  check(c1.length === 0, 'POV does not slice with zero observed volume');
  algo.observeVolume(50);
  const c2 = algo.step(1);
  check(c2.length === 1 && Math.abs(c2[0].qty - 5) < 1e-9, 'POV slices 10% of observed');
}

console.log('=== Validation ===');
{
  let threw = false;
  try { new flox.TWAPExecutor({ targetQty: -1, side: 'buy', durationNs: 1, sliceCount: 1, startTimeNs: 0 }); }
  catch (e) { threw = true; check(e.code === 'E_VAL_002', 'negative targetQty rejected'); }
  check(threw, 'TWAP negative qty raises');
}

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
