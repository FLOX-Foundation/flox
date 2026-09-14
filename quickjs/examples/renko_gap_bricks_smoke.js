// Cross-binding parity check for Renko gap-brick synthesis.
// Mirrors python/tests/test_renko_gap_bricks.py and
// node/test/test_renko_gap_bricks.js: a trade that gaps past more than one
// brick width used to collapse into a single zero-range bar, silently
// dropping every brick in between. flox.renkoBars() goes through the same
// shared C ABI aggregator Node does (flox_aggregate_renko_bars), which
// never flushes a still-open trailing bar -- so, like the Node test, this
// expects 5 bars for the gap case, not 6.
//
// This also doubles as the QuickJS regression check for doAgg() sizing its
// output buffer to the trade count: Renko can now emit more bars than
// there were trades, and the old code silently padded the shortfall with
// zero-filled bars instead of the real ones that did not fit -- see the
// doAgg() comment in src/quickjs/js_bindings.cpp.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

{
    const ts = [0, 1000000000];
    const px = [100.0, 155.0];  // gaps 5.5 bricks past 100
    const qty = [1.0, 1.0];
    const sides = [1, 1];

    const bars = flox.renkoBars(ts, px, qty, sides, 10.0);

    check(bars.length === 5, '1 real bar + 4 synthesized bricks, got ' + bars.length);
    check(bars[0].open === 100 && bars[0].close === 100, 'real bar stays at 100 (no trade touched a price in between)');

    const expectedOpens = [110, 120, 130, 140];
    const expectedCloses = [120, 130, 140, 150];
    for (let i = 0; i < 4; i++) {
        check(bars[i + 1].open === expectedOpens[i], 'brick ' + i + ' open === ' + expectedOpens[i] + ', got ' + bars[i + 1].open);
        check(bars[i + 1].close === expectedCloses[i], 'brick ' + i + ' close === ' + expectedCloses[i] + ', got ' + bars[i + 1].close);
    }
}

{
    const ts = [0, 1000000000];
    const px = [100.0, 114.0];  // 1.4 bricks -- one whole brick, no gap synthesis
    const qty = [1.0, 1.0];
    const sides = [1, 1];

    const bars = flox.renkoBars(ts, px, qty, sides, 10.0);

    check(bars.length === 1, 'ordinary single-brick close is unaffected, got ' + bars.length);
    check(bars[0].open === 100, 'brick opens at 100');
}

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('renko gap-brick synthesis smoke failed');
}
console.log('\n' + passed + ' check(s) passed');
