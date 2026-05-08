// Smoke test for flox.tapeDiff exposed on the flox global.
// Writes two .floxlog tapes via flox.DataWriter, then exercises:
//   - identical tapes return equal=true with no mismatches
//   - one diverging record is reported with the right index
//   - prefix-shorter tape sets firstDivergenceIndex to len(shorter)
//   - bad path raises an error

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

// DataWriter writeTrade signature mirrors the C ABI order:
//   (timestampNs, exchangeNs, price, qty, tradeId, symbolId, isBuy)
function makeTape(dir, trades) {
    var w = new DataWriter(dir, 0, 0);
    for (var i = 0; i < trades.length; i++) {
        var t = trades[i];
        w.writeTrade(t.tsNs, t.tsNs, t.price, t.qty,
                     t.tradeId, t.symbolId, t.side === 'buy');
    }
    w.destroy();
}

var tmpRoot = '/tmp/flox-tape-diff-qjs-smoke-' + Date.now() + '-' + Math.floor(Math.random() * 1000000);
__flox_load_csv;  // touch any C ABI to ensure host is wired

var leftDir = tmpRoot + '/left';
var rightDir = tmpRoot + '/right';
var shortDir = tmpRoot + '/short';

var base = [
    { tsNs: 1000, price: 100.0, qty: 1.0, tradeId: 1, symbolId: 1, side: 'buy' },
    { tsNs: 2000, price: 101.0, qty: 1.0, tradeId: 2, symbolId: 1, side: 'sell' },
    { tsNs: 3000, price: 102.0, qty: 2.0, tradeId: 3, symbolId: 1, side: 'buy' },
];
var diverging = [
    { tsNs: 1000, price: 100.0, qty: 1.0, tradeId: 1, symbolId: 1, side: 'buy' },
    { tsNs: 2000, price: 999.0, qty: 1.0, tradeId: 2, symbolId: 1, side: 'sell' },
    { tsNs: 3000, price: 102.0, qty: 2.0, tradeId: 3, symbolId: 1, side: 'buy' },
];

makeTape(leftDir, base);
makeTape(rightDir, diverging);
makeTape(shortDir, base.slice(0, 2));

// Identical
var r1 = flox.tapeDiff(leftDir, leftDir);
check(r1.equal === true, 'identical: equal=true');
check(r1.firstDivergenceIndex === null, 'identical: firstDivergenceIndex null');
check(r1.mismatches.length === 0, 'identical: no mismatches');

// Diverging
var r2 = flox.tapeDiff(leftDir, rightDir);
check(r2.equal === false, 'diverging: equal=false');
check(r2.firstDivergenceIndex === 1, 'diverging: index 1');
check(r2.mismatches.length === 1, 'diverging: one mismatch');
check(r2.mismatches[0].index === 1, 'diverging: mismatch index 1');
check(r2.mismatches[0].left.priceRaw !== r2.mismatches[0].right.priceRaw,
      'diverging: prices differ in mismatch payload');

// Prefix shorter
var r3 = flox.tapeDiff(leftDir, shortDir);
check(r3.equal === false, 'shorter: equal=false');
check(r3.firstDivergenceIndex === 2, 'shorter: divergence at len(shorter)=2');

// Invalid path
var threw = false;
try { flox.tapeDiff('/tmp/does-not-exist-xyz-qjs', leftDir); }
catch (e) { threw = true; }
check(threw, 'invalid path rejected');

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('tape diff smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
