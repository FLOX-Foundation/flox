'use strict';
/**
 * node/test/test_composite_book.js — CompositeBookMatrix ingestion.
 *
 * Node had no way to feed a CompositeBookMatrix at all: the C ABI it wraps
 * exported create/destroy/best-price/arbitrage/staleness but no update
 * entry point, so a matrix built here stayed empty forever. This exercises
 * the new applySnapshot/applyDelta methods end to end.
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

console.log('=== Snapshot populates both sides ===');
{
  const book = new flox.CompositeBookMatrix();
  check(book.bestBid(1) === null, 'empty book has no bid');

  book.applySnapshot(
    1, 1,
    new Float64Array([100.0]), new Float64Array([1.5]),
    new Float64Array([100.02]), new Float64Array([0.5]),
    1000n,
  );

  const bid = book.bestBid(1);
  const ask = book.bestAsk(1);
  check(bid !== null && Math.abs(bid.price - 100.0) < 1e-9, 'best bid price after snapshot');
  check(bid !== null && Math.abs(bid.qty - 1.5) < 1e-9, 'best bid qty after snapshot');
  check(ask !== null && Math.abs(ask.price - 100.02) < 1e-9, 'best ask price after snapshot');
}

console.log('=== Bid-only delta does not wipe the ask side ===');
{
  const book = new flox.CompositeBookMatrix();
  book.applySnapshot(
    1, 7,
    new Float64Array([100.0]), new Float64Array([1.0]),
    new Float64Array([100.05]), new Float64Array([2.0]),
    0n,
  );
  book.applyDelta(
    1, 7,
    new Float64Array([100.01]), new Float64Array([3.0]),
    new Float64Array([]), new Float64Array([]),
    0n,
  );

  const bid = book.bestBid(7);
  const ask = book.bestAsk(7);
  check(bid !== null && Math.abs(bid.price - 100.01) < 1e-9, 'bid moved to the delta price');
  check(ask !== null && Math.abs(ask.price - 100.05) < 1e-9, 'ask side untouched by a bid-only delta');
}

console.log('=== Arbitrage across two exchanges ===');
{
  const book = new flox.CompositeBookMatrix();
  book.applySnapshot(1, 3, new Float64Array([101.0]), new Float64Array([1.0]),
                     new Float64Array([101.5]), new Float64Array([1.0]), 0n);
  book.applySnapshot(2, 3, new Float64Array([102.0]), new Float64Array([1.0]),
                     new Float64Array([103.0]), new Float64Array([1.0]), 0n);
  check(book.hasArbitrage(3) === true, 'a higher bid on exchange 2 than the ask on exchange 1 is arbitrage');
}

console.log('=== Mismatched price/qty array lengths are rejected ===');
{
  const book = new flox.CompositeBookMatrix();
  let threw = false;
  try {
    book.applySnapshot(1, 1, new Float64Array([100.0, 99.0]), new Float64Array([1.0]),
                       new Float64Array([]), new Float64Array([]), 0n);
  } catch (e) {
    threw = true;
  }
  check(threw, 'mismatched bid price/qty lengths throw rather than read out of bounds');
}

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
