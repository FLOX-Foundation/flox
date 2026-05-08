'use strict';
/**
 * node/test/test_latency.js — smoke-test latency model NAPI bindings.
 *
 * Run from repo root:
 *   cd node && node test/test_latency.js
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;

function check(condition, msg) {
  if (condition) {
    console.log(`  ok  ${msg}`);
    passed++;
  } else {
    console.error(`  FAIL  ${msg}`);
    failed++;
  }
}

// ── ConstantLatency ────────────────────────────────────────────────

console.log('=== ConstantLatency ===');
{
  const c = new flox.ConstantLatency({ feedNs: 100, orderNs: 200, fillNs: 300 });
  check(c.feedDelay() === 100, 'feed_delay returns configured feed_ns');
  check(c.orderDelay() === 200, 'order_delay returns configured order_ns');
  check(c.fillDelay() === 300, 'fill_delay returns configured fill_ns');
  const s = c.sample();
  check(s.feedNs === 100 && s.orderNs === 200 && s.fillNs === 300,
        'sample returns LatencySample with all three');
  // Two calls are byte-identical for constant.
  check(c.feedDelay() === c.feedDelay(), 'constant is deterministic');
}
{
  let threw = false;
  try { new flox.ConstantLatency({ feedNs: -1 }); }
  catch (e) { threw = true; check(e.code === 'E_VAL_002', 'negative feedNs throws E_VAL_002'); }
  check(threw, 'negative feedNs rejected');
}

// ── GaussianLatency ────────────────────────────────────────────────

console.log('=== GaussianLatency ===');
{
  const g = new flox.GaussianLatency({
    feedMeanNs: 1000, feedStddevNs: 200,
    orderMeanNs: 500, orderStddevNs: 50,
    fillMeanNs: 2000, fillStddevNs: 300,
    seed: 42,
  });
  // Reproducibility under the same seed.
  const a = [g.feedDelay(), g.orderDelay(), g.fillDelay()];
  g.reset(42);
  const b = [g.feedDelay(), g.orderDelay(), g.fillDelay()];
  check(a[0] === b[0] && a[1] === b[1] && a[2] === b[2],
        'gaussian sequence is reproducible under reset(seed)');
  // Non-negativity.
  let allNonNeg = true;
  for (let i = 0; i < 50; i++) {
    if (g.feedDelay() < 0 || g.orderDelay() < 0 || g.fillDelay() < 0) {
      allNonNeg = false; break;
    }
  }
  check(allNonNeg, 'gaussian samples are clamped non-negative');
  // Stddev=0 collapses to mean.
  const collapsed = new flox.GaussianLatency({ feedMeanNs: 1500, feedStddevNs: 0, seed: 0 });
  check(collapsed.feedDelay() === 1500, 'stddev=0 collapses to mean');
}

// ── ExponentialLatency ─────────────────────────────────────────────

console.log('=== ExponentialLatency ===');
{
  const e = new flox.ExponentialLatency({ feedMeanNs: 500, seed: 7 });
  let nonNegSeen = true;
  for (let i = 0; i < 50; i++) {
    if (e.feedDelay() < 0) { nonNegSeen = false; break; }
  }
  check(nonNegSeen, 'exponential samples are non-negative');
  const zero = new flox.ExponentialLatency({ feedMeanNs: 0, seed: 0 });
  check(zero.feedDelay() === 0, 'zero mean -> zero delay');
}

// ── EmpiricalLatency ───────────────────────────────────────────────

console.log('=== EmpiricalLatency ===');
{
  const emp = new flox.EmpiricalLatency({ feedSamples: [10, 20, 30], seed: 0 });
  const allowed = new Set([10, 20, 30]);
  let allInSet = true;
  for (let i = 0; i < 30; i++) {
    if (!allowed.has(emp.feedDelay())) { allInSet = false; break; }
  }
  check(allInSet, 'empirical draws are subset of input samples');
  check(emp.orderDelay() === 0, 'empirical with no order_samples returns 0');
}
{
  let threw = false;
  try { new flox.EmpiricalLatency({}); }
  catch (e) { threw = true; check(e.code === 'E_VAL_002', 'empty empirical throws E_VAL_002'); }
  check(threw, 'all-empty empirical rejected');
}

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
