// Smoke test for flox.DeltaBookEncoder / DeltaBookReplayer.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

function lvl(price, qty) { return { priceRaw: price, qtyRaw: qty }; }

var enc = new flox.DeltaBookEncoder({ anchorEvery: 100 });
var first = enc.encode(1, [lvl(10000, 10), lvl(9999, 5)], [lvl(10001, 8)]);
check(first.isDelta === false, 'first event is a snapshot');
check(first.bids.length === 2 && first.asks.length === 1, 'snapshot carries levels');

var second = enc.encode(1, [lvl(10000, 12)], [lvl(10001, 8), lvl(10002, 3)]);
check(second.isDelta === true, 'second event is a delta');

var rep = new flox.DeltaBookReplayer();
rep.apply(0, 1, [lvl(10000, 10), lvl(9999, 5)], [lvl(10001, 8)]);
var snap = rep.apply(1, 1, second.bids, second.asks);
var bidPrices = snap.bids.map(function(l) { return l.priceRaw; }).sort();
check(bidPrices.length === 1 && bidPrices[0] === 10000,
      'replayer drops removed prices and keeps changed ones');
var bidQty = snap.bids[0].qtyRaw;
check(bidQty === 12, 'replayer reflects updated quantity');

enc.destroy();
rep.destroy();

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('delta book smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
