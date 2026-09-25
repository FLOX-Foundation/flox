'use strict';
/**
 * node/test/test_run_bars_array_lengths.js -- BacktestRunner.runBars and
 * .runOhlcv reject mismatched input columns before any call into C.
 *
 * runBars (node/src/strategy.h) derived its element count from
 * startNs.ElementLength() alone and handed the other seven .Data()
 * pointers straight to flox_backtest_runner_run_bars, which takes one
 * uint32_t n and indexes all eight arrays over [0, n). A `high` column
 * one element short was an out-of-bounds read of the other seven, and the
 * run completed and reported stats as if nothing had happened. runOhlcv
 * has the same shape with two columns.
 *
 * node/src/bindings_common.h::requireSameLength was written for exactly
 * this and is already used by indicators.h, aggregators.h and stats.h;
 * node/test/test_array_length_safety.js is its existing Node-side pin for
 * the aggregator path. The rejection has to happen before the C call, so
 * a mismatch delivers no bars and no trades at all rather than a partial
 * run over garbage.
 *
 * The thrown error is whichever of RangeError / TypeError the binding
 * uses for a bad argument -- requireSameLength raises a RangeError today
 * -- and its message names the method so the caller knows which call site
 * to look at.
 *
 * Run from repo root:
 *   cd node && node test/test_run_bars_array_lengths.js
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const N = 3;

function makeRunner() {
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.BacktestRunner(reg, 0.0, 10000.0);
  const seenBars = [];
  const seenTrades = [];
  runner.setStrategy({
    symbols: [sym],
    onBar(_ctx, bar) { seenBars.push(bar); },
    onTrade(_ctx, trade) { seenTrades.push(trade); },
  });
  return { runner, seenBars, seenTrades };
}

// Full-length columns, in the argument order runBars takes them.
function barColumns() {
  const startNs = new BigInt64Array(N);
  const endNs = new BigInt64Array(N);
  const open = new Float64Array(N);
  const high = new Float64Array(N);
  const low = new Float64Array(N);
  const close = new Float64Array(N);
  const volume = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    startNs[i] = BigInt(i) * 1000000000n;
    endNs[i] = startNs[i] + 999999999n;
    open[i] = 100 + i;
    high[i] = 101 + i;
    low[i] = 99 + i;
    close[i] = 100.5 + i;
    volume[i] = 10;
  }
  return [startNs, endNs, open, high, low, close, volume];
}

const BAR_COLUMN_NAMES = ['startNs', 'endNs', 'open', 'high', 'low', 'close', 'volume'];

// Drop the last element of one column, keeping its type.
function shortened(arr) {
  return arr.subarray(0, arr.length - 1);
}

function captureThrow(fn) {
  try { fn(); return null; } catch (e) { return e; }
}

function checkRejection(label, method, err, seenBars, seenTrades) {
  check(err !== null, `${label}: throws instead of running (got no throw)`);
  if (err === null) { return; }
  check(err instanceof RangeError || err instanceof TypeError,
        `${label}: throws a RangeError or TypeError (got ${err.constructor.name})`);
  check(String(err.message).includes(method),
        `${label}: message names ${method} (message was "${err.message}")`);
  check(/length/i.test(String(err.message)),
        `${label}: message names the length mismatch (message was "${err.message}")`);
  check(seenBars.length === 0 && seenTrades.length === 0,
        `${label}: nothing was dispatched (bars=${seenBars.length}, trades=${seenTrades.length})`);
}

console.log('=== runBars rejects a short column, one column at a time ===');
for (let i = 0; i < BAR_COLUMN_NAMES.length; i++) {
  const { runner, seenBars, seenTrades } = makeRunner();
  const cols = barColumns();
  cols[i] = shortened(cols[i]);
  const err = captureThrow(() => runner.runBars(...cols, 'BTC'));
  checkRejection(`runBars with a short ${BAR_COLUMN_NAMES[i]}`, 'runBars', err, seenBars, seenTrades);
}

console.log('\n=== runBars still runs when every column matches ===');
{
  const { runner, seenBars } = makeRunner();
  const cols = barColumns();
  const stats = runner.runBars(...cols, 'BTC');
  check(stats !== null && typeof stats === 'object',
        `runBars returned a stats object (got ${stats === null ? 'null' : typeof stats})`);
  check(seenBars.length === N, `every bar was delivered (got ${seenBars.length}, want ${N})`);
}

console.log('\n=== runBars accepts the optional barType / barTypeParam tail ===');
{
  const { runner, seenBars } = makeRunner();
  const cols = barColumns();
  runner.runBars(...cols, 'BTC', 0, 1000000000);
  check(seenBars.length === N,
        `every bar was delivered with the optional tail (got ${seenBars.length}, want ${N})`);
}

console.log('\n=== runOhlcv rejects a mismatched close column ===');
{
  const { runner, seenBars, seenTrades } = makeRunner();
  const ts = new BigInt64Array([0n, 1000000000n, 2000000000n]);
  const close = new Float64Array([100.0]);
  const err = captureThrow(() => runner.runOhlcv(ts, close, 'BTC'));
  checkRejection('runOhlcv with a short close', 'runOhlcv', err, seenBars, seenTrades);
}

console.log('\n=== runOhlcv rejects a mismatched timestamp column ===');
{
  const { runner, seenBars, seenTrades } = makeRunner();
  const ts = new BigInt64Array([0n]);
  const close = new Float64Array([100.0, 101.0, 102.0]);
  const err = captureThrow(() => runner.runOhlcv(ts, close, 'BTC'));
  checkRejection('runOhlcv with a short timestamp column', 'runOhlcv', err, seenBars, seenTrades);
}

console.log('\n=== runOhlcv still runs when both columns match ===');
{
  const { runner, seenTrades } = makeRunner();
  const ts = new BigInt64Array([0n, 1000000000n, 2000000000n]);
  const close = new Float64Array([100.0, 101.0, 102.0]);
  const stats = runner.runOhlcv(ts, close, 'BTC');
  check(stats !== null && typeof stats === 'object',
        `runOhlcv returned a stats object (got ${stats === null ? 'null' : typeof stats})`);
  check(seenTrades.length === 3, `every row was delivered (got ${seenTrades.length}, want 3)`);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
