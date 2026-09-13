// OrderBook tick handling and delta-fed window tracking through the Node
// binding, which reads the same C++ book the engine does.

const assert = require('assert');
const flox = require('../index.js');

let passed = 0;
function check(cond, msg) {
  assert.ok(cond, msg);
  passed++;
}
function throws(fn, msg) {
  let threw = null;
  try { fn(); } catch (e) { threw = e; }
  assert.ok(threw, msg + ' (expected a throw, got none)');
  passed++;
}
function near(a, b, eps, msg) {
  assert.ok(Math.abs(a - b) <= eps, `${msg} (got ${a}, want ${b})`);
  passed++;
}

// ── Tick size validation ──────────────────────────────────────────────
// A zero tick divided by zero while building the reciprocal and left a book
// that was dead for the rest of its life without a single diagnostic.
throws(() => new flox.OrderBook(0), 'zero tick size is rejected');
throws(() => new flox.OrderBook(-0.01), 'negative tick size is rejected');
check(new flox.OrderBook(0.01) !== null, 'a positive tick size is accepted');

// ── Sub-cent tick sizes ───────────────────────────────────────────────
// A 1e-8 tick is a raw divisor of 1 at a price scale of 1e8. Every level used
// to collapse onto one index, which left the book permanently crossed.
{
  const book = new flox.OrderBook(1e-8);
  book.applySnapshot(Float64Array.from([1e-5]), Float64Array.from([1.0]),
                     Float64Array.from([1.002e-5]), Float64Array.from([1.0]));
  near(book.bestBid(), 1e-5, 1e-14, 'best bid at a 1e-8 tick');
  near(book.bestAsk(), 1.002e-5, 1e-14, 'best ask at a 1e-8 tick');
  check(book.isCrossed() === false, 'a normal book is not crossed at a 1e-8 tick');
  near(book.mid(), 1.001e-5, 1e-14, 'mid at a 1e-8 tick');
  book.clear();
}

// ── Delta-only drift ──────────────────────────────────────────────────
// A delta feed re-sends a snapshot only on connect and on a sequence gap, so
// the tick window has to follow the market between the two. It did not: about
// $25 of movement emptied one side of the book and $30 emptied both.
{
  const tick = 0.1;
  const depth = 10;
  const book = new flox.OrderBook(tick);
  const bidTop = 59999.9;
  const askTop = 60000.1;

  const round8 = (v) => Math.round(v * 1e8) / 1e8;
  const bids = [], asks = [], qty = [];
  for (let k = 0; k < depth; ++k) {
    bids.push(round8(bidTop - k * tick));
    asks.push(round8(askTop + k * tick));
    qty.push(1.0);
  }
  book.applySnapshot(Float64Array.from(bids), Float64Array.from(qty),
                     Float64Array.from(asks), Float64Array.from(qty));

  let lastBid = 0, lastAsk = 0;
  for (let step = 1; step <= 600; ++step) {
    const newBid = round8(bidTop + step * tick);
    const newAsk = round8(askTop + step * tick);
    book.applyDelta(
      Float64Array.from([newBid, round8(newBid - depth * tick)]),
      Float64Array.from([1.0, 0.0]),
      Float64Array.from([round8(newAsk + (depth - 1) * tick), round8(newAsk - tick)]),
      Float64Array.from([1.0, 0.0]));

    assert.ok(book.bestBid() !== null, `bid side went dark at step ${step}`);
    assert.ok(book.bestAsk() !== null, `ask side went dark at step ${step}`);
    lastBid = book.bestBid();
    lastAsk = book.bestAsk();
    assert.ok(Math.abs(lastBid - newBid) < 1e-6, `best bid wrong at step ${step}`);
    assert.ok(Math.abs(lastAsk - newAsk) < 1e-6, `best ask wrong at step ${step}`);
  }
  passed++;
  near(lastBid, round8(bidTop + 600 * tick), 1e-6, 'best bid after $60 of drift');
  near(lastAsk, round8(askTop + 600 * tick), 1e-6, 'best ask after $60 of drift');
  book.clear();
}

console.log(`test_order_book_window: ${passed} checks passed`);
