'use strict';
// The context quotes say "no quote" as null, and a price of 0 as 0.
//
// ctx.bestBid / bestAsk / midPrice used to be Numbers that read 0 for an
// empty side, the number a bid at exactly 0 also reads, so a strategy could
// not tell the two apart. The C API snapshot carries presence flags now, and
// this is the Node surface reading them.

const assert = require('node:assert');
const path = require('path');
const flox = require(path.join(__dirname, '..'));

const reg = new flox.SymbolRegistry();
const sym = reg.addSymbol('test', 'ZERO', 0.01);

const seen = [];
const runner = new flox.Runner(reg, () => {}, false);
runner.addStrategy({
  symbols: [Number(sym)],
  onBookUpdate(ctx) {
    seen.push({ bid: ctx.bestBid, ask: ctx.bestAsk, mid: ctx.midPrice });
  },
});
runner.start();

function push(bids, asks, ts) {
  runner.onBookSnapshot(
    Number(sym),
    Float64Array.from(bids.map((l) => l[0])), Float64Array.from(bids.map((l) => l[1])),
    Float64Array.from(asks.map((l) => l[0])), Float64Array.from(asks.map((l) => l[1])),
    BigInt(ts),
  );
}
function last() {
  assert.ok(seen.length > 0, 'onBookUpdate never fired');
  return seen[seen.length - 1];
}

// An empty book: nothing to quote on either side.
push([], [], 1);
let s = last();
assert.strictEqual(s.bid, null, 'bestBid on an empty book must be null');
assert.strictEqual(s.ask, null, 'bestAsk on an empty book must be null');
assert.strictEqual(s.mid, null, 'midPrice on an empty book must be null');

// A bid at exactly 0 and no ask: the bid is a price, the ask is absent.
push([[0.0, 1.0]], [], 2);
s = last();
assert.strictEqual(typeof s.bid, 'number', 'a bid at exactly 0 must read as a number');
assert.strictEqual(s.bid, 0);
assert.strictEqual(s.ask, null, 'the missing ask must read as null, not 0');
assert.strictEqual(s.mid, null, 'no mid without both sides');

// An ask at exactly 0 and no bid.
push([], [[0.0, 1.0]], 3);
s = last();
assert.strictEqual(s.ask, 0);
assert.strictEqual(s.bid, null);
assert.strictEqual(s.mid, null);

// A book straddling zero: every field is a number, the mid is exactly 0.
push([[-0.01, 1.0]], [[0.01, 1.0]], 4);
s = last();
assert.ok(Math.abs(s.bid - -0.01) < 1e-9, `bid ${s.bid}`);
assert.ok(Math.abs(s.ask - 0.01) < 1e-9, `ask ${s.ask}`);
assert.strictEqual(typeof s.mid, 'number', 'a mid of exactly 0 must read as a number');
assert.strictEqual(s.mid, 0);

// A side that goes away reads as null again: no stale price survives.
push([[100.0, 2.0]], [[100.05, 1.0]], 5);
push([[100.0, 2.0]], [], 6);
s = last();
assert.ok(Math.abs(s.bid - 100.0) < 1e-9);
assert.strictEqual(s.ask, null, 'the ask side emptied');
assert.strictEqual(s.mid, null);

runner.stop();
console.log('test_symbol_context_quotes: ok');
