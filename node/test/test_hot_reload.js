'use strict';
/**
 * node/test/test_hot_reload.js — Strategy hot-reload via runner.replaceStrategy.
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const reg = new flox.SymbolRegistry();
const sym = Number(reg.addSymbol('test', 'BTCUSDT', 0.01));

function recordingStrategy(label) {
  const s = {
    label,
    symbols: [sym],
    starts: 0,
    stops: 0,
    trades: [],
  };
  s.onStart = () => { s.starts++; };
  s.onStop = () => { s.stops++; };
  s.onTrade = (_ctx, trade) => { s.trades.push([s.label, trade.price]); };
  return s;
}

console.log('=== replaceStrategy swaps callbacks ===');
{
  const runner = new flox.Runner(reg, () => {}, false);
  const a = recordingStrategy('A');
  runner.addStrategy(a);
  runner.start();
  runner.onTrade(sym, 100, 1, true, 1_000_000);
  check(a.trades.length === 1 && a.trades[0][0] === 'A',
        'A receives first trade');

  const b = recordingStrategy('B');
  runner.replaceStrategy(0, b);
  runner.onTrade(sym, 101, 1, false, 2_000_000);
  runner.stop();

  check(a.trades.length === 1, 'A is unchanged after replace');
  check(b.trades.length === 1 && b.trades[0][1] === 101,
        'B receives the second trade with the right price');
  check(a.starts === 1 && a.stops === 1, 'A start/stop balanced');
  check(b.starts === 1 && b.stops === 1, 'B start/stop balanced');
}

console.log('=== replaceStrategy rejects bad index ===');
{
  const runner = new flox.Runner(reg, () => {}, false);
  runner.addStrategy(recordingStrategy('A'));
  let threw = false;
  try { runner.replaceStrategy(99, recordingStrategy('B')); }
  catch (e) { threw = true; check(e.code === 'E_VAL_002', 'bad index throws E_VAL_002'); }
  check(threw, 'bad index rejected');
}

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
