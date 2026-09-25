'use strict';
/**
 * node/test/test_backtest_timestamps.js — regression test:
 * BacktestRunner.runBars / .runOhlcv document their timestamps as already
 * nanoseconds (see node/index.d.ts), but the shared C ABI they call into
 * (flox_backtest_runner_run_bars / run_ohlcv in src/capi/flox_capi.cpp)
 * used to run every one of them back through a unit-guessing helper meant
 * for CSV columns, where the unit really is unknown. Any nanosecond value
 * under 1e12 -- an ordinary timestamp for synthetic test data built from
 * small offsets -- read as "probably seconds" and got rescaled by another
 * 1e9, corrupting it. Fixed once in the C interface; Node and Codon both
 * call straight through it, so no binding-side change was needed here --
 * this just proves the fix reaches Node.
 *
 * Run from repo root:
 *   cd node && node test/test_backtest_timestamps.js
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

// 60 seconds since the epoch, in nanoseconds -- under the old 1e12
// threshold that meant "probably seconds, scale up".
const START_NS = 60_000_000_000n;
const END_NS = 119_999_999_999n;

console.log('=== BacktestRunner.runBars passes nanoseconds through unmodified ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const btr = new flox.BacktestRunner(reg, 0.0, 10000.0);

  const seenBars = [];
  btr.setStrategy({
    symbols: [sym],
    onBar(_ctx, bar) { seenBars.push(bar); },
  });

  btr.runBars(
    new BigInt64Array([START_NS]),
    new BigInt64Array([END_NS]),
    new Float64Array([100.0]),
    new Float64Array([101.0]),
    new Float64Array([99.0]),
    new Float64Array([100.5]),
    new Float64Array([10.0]),
    'BTC');

  check(seenBars.length === 1, `exactly one bar delivered (got ${seenBars.length})`);
  // Bar timestamps cross as BigInt -- see test_bar_timestamp_bigint.js.
  check(seenBars[0]?.startTimeNs === START_NS,
        `startTimeNs unmodified (got ${seenBars[0]?.startTimeNs}, want ${START_NS})`);
  check(seenBars[0]?.endTimeNs === END_NS,
        `endTimeNs unmodified (got ${seenBars[0]?.endTimeNs}, want ${END_NS})`);
}

console.log('\n=== BacktestRunner.runOhlcv passes nanoseconds through unmodified ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const btr = new flox.BacktestRunner(reg, 0.0, 10000.0);

  const seenTrades = [];
  btr.setStrategy({
    symbols: [sym],
    onTrade(_ctx, trade) { seenTrades.push(trade); },
  });

  btr.runOhlcv(new BigInt64Array([START_NS]), new Float64Array([100.5]), 'BTC');

  check(seenTrades.length === 1, `exactly one trade delivered (got ${seenTrades.length})`);
  // timestampNs comes back as a bigint (see node/src/strategy.h).
  check(seenTrades[0]?.timestampNs === START_NS,
        `timestampNs unmodified (got ${seenTrades[0]?.timestampNs}, want ${START_NS})`);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
