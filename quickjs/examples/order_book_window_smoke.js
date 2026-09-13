// Smoke test for flox.OrderBook: tick validation, sub-cent ticks, and the
// tick window following a delta-only feed.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}
function near(a, b, eps, msg) {
    check(Math.abs(a - b) <= eps, msg + ' (got ' + a + ', want ' + b + ')');
}
function throws(fn, msg) {
    var threw = false;
    try { fn(); } catch (e) { threw = true; }
    check(threw, msg);
}

// A zero tick divided by zero while building the reciprocal and left a book
// that was dead for the rest of its life without a single diagnostic.
throws(function() { new OrderBook(0); }, 'zero tick size is rejected');
throws(function() { new OrderBook(-0.01); }, 'negative tick size is rejected');

// A 1e-8 tick is a raw divisor of 1 at a price scale of 1e8. Every level used
// to collapse onto one index, which left the book permanently crossed.
var tiny = new OrderBook(1e-8);
tiny.applySnapshot([1e-5], [1.0], [1.002e-5], [1.0]);
near(tiny.bestBid(), 1e-5, 1e-14, 'best bid at a 1e-8 tick');
near(tiny.bestAsk(), 1.002e-5, 1e-14, 'best ask at a 1e-8 tick');
check(tiny.isCrossed() === false, 'a normal book is not crossed at a 1e-8 tick');
near(tiny.mid(), 1.001e-5, 1e-14, 'mid at a 1e-8 tick');
tiny.destroy();

// A delta feed re-sends a snapshot only on connect and on a sequence gap, so
// the tick window has to follow the market between the two.
var tick = 0.1;
var depth = 10;
var book = new OrderBook(tick);
var bidTop = 59999.9;
var askTop = 60000.1;

function r8(v) { return Math.round(v * 1e8) / 1e8; }

var bids = [], asks = [], qty = [];
for (var k = 0; k < depth; ++k) {
    bids.push(r8(bidTop - k * tick));
    asks.push(r8(askTop + k * tick));
    qty.push(1.0);
}
book.applySnapshot(bids, qty, asks, qty);

var alive = true;
var lastBid = 0, lastAsk = 0;
for (var step = 1; step <= 600; ++step) {
    var newBid = r8(bidTop + step * tick);
    var newAsk = r8(askTop + step * tick);
    book.applyDelta([newBid, r8(newBid - depth * tick)], [1.0, 0.0],
                    [r8(newAsk + (depth - 1) * tick), r8(newAsk - tick)], [1.0, 0.0]);
    lastBid = book.bestBid();
    lastAsk = book.bestAsk();
    if (lastBid === null || lastAsk === null ||
        Math.abs(lastBid - newBid) > 1e-6 || Math.abs(lastAsk - newAsk) > 1e-6) {
        alive = false;
        console.log('  book went wrong at step ' + step);
        break;
    }
}
check(alive, 'book tracks 600 ticks of delta-only drift');
near(lastBid, r8(bidTop + 600 * tick), 1e-6, 'best bid after $60 of drift');
near(lastAsk, r8(askTop + 600 * tick), 1e-6, 'best ask after $60 of drift');
book.destroy();

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('order book window smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
