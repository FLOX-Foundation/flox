'use strict';
/**
 * node/test/test_tape_diff.js — smoke-test the C++-backed flox.tapeDiff.
 *
 * Builds two .floxlog tapes via DataWriter, then exercises:
 *   - identical tapes return equal=true with no mismatches
 *   - one diverging record is reported with the right index + payloads
 *   - prefix-shorter tape sets firstDivergenceIndex to len(shorter)
 *   - bad path raises FloxError with E_IO_001
 *
 * Run from repo root: cd node && node test/test_tape_diff.js
 */

const path = require('path');
const fs = require('fs');
const os = require('os');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { console.log(`  ok  ${msg}`); passed++; }
  else { console.error(`  FAIL  ${msg}`); failed++; }
}

function makeTape(dir, trades) {
  fs.mkdirSync(dir, { recursive: true });
  const w = new flox.DataWriter(dir, 1, 0);
  for (const t of trades) {
    const sideInt = t.side === 'buy' ? 0 : 1;
    w.writeTrade(t.tsNs, t.tsNs, t.price, t.qty, t.tradeId, t.symbolId, sideInt);
  }
  w.flush();
  w.close();
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'tape-diff-'));
const left = path.join(tmpRoot, 'left');
const right = path.join(tmpRoot, 'right');
const shorter = path.join(tmpRoot, 'shorter');

const baseTrades = [
  { tsNs: 1_000, price: 100.0, qty: 1.0, tradeId: 1, symbolId: 1, side: 'buy' },
  { tsNs: 2_000, price: 101.0, qty: 1.0, tradeId: 2, symbolId: 1, side: 'sell' },
  { tsNs: 3_000, price: 102.0, qty: 2.0, tradeId: 3, symbolId: 1, side: 'buy' },
];
const divergingTrades = [
  { tsNs: 1_000, price: 100.0, qty: 1.0, tradeId: 1, symbolId: 1, side: 'buy' },
  // Different price at index 1 — the divergence we expect to land on.
  { tsNs: 2_000, price: 999.0, qty: 1.0, tradeId: 2, symbolId: 1, side: 'sell' },
  { tsNs: 3_000, price: 102.0, qty: 2.0, tradeId: 3, symbolId: 1, side: 'buy' },
];
const shorterTrades = baseTrades.slice(0, 2);

makeTape(left, baseTrades);
makeTape(right, divergingTrades);
makeTape(shorter, shorterTrades);

console.log('=== identical tapes ===');
{
  const r = flox.tapeDiff(left, left);
  check(r.equal === true, 'equal true for self-comparison');
  check(r.firstDivergenceIndex === null, 'firstDivergenceIndex null when equal');
  check(r.mismatches.length === 0, 'no mismatches recorded');
  check(r.leftCount === 3 && r.rightCount === 3, 'counts match');
}

console.log('=== diverging tape ===');
{
  const r = flox.tapeDiff(left, right);
  check(r.equal === false, 'equal false when records differ');
  check(r.firstDivergenceIndex === 1, 'firstDivergenceIndex points at index 1');
  check(r.mismatches.length === 1, 'one mismatch recorded');
  const m = r.mismatches[0];
  check(m.index === 1, 'mismatch index === 1');
  check(m.left.priceRaw !== m.right.priceRaw, 'mismatch left/right price differ');
}

console.log('=== prefix shorter ===');
{
  const r = flox.tapeDiff(left, shorter);
  check(r.equal === false, 'equal false when lengths differ');
  check(r.leftCount === 3 && r.rightCount === 2, 'counts reflect both tapes');
  check(r.firstDivergenceIndex === 2, 'firstDivergenceIndex set to shorter length');
}

console.log('=== invalid path ===');
{
  let threw = false;
  try { flox.tapeDiff('/tmp/does-not-exist-xyz', left); }
  catch (e) { threw = true; check(e.code === 'E_IO_001', 'invalid path throws E_IO_001'); }
  check(threw, 'invalid path rejected');
}

fs.rmSync(tmpRoot, { recursive: true, force: true });

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
