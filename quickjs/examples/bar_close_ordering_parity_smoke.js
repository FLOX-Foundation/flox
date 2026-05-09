// Cross-binding parity test for bar-close dispatch ordering.
// Mirrors python/tests/test_bar_close_ordering_parity.py and
// node/test/test_bar_close_ordering_parity.js. All four bindings
// must produce the same dispatch sequence on the same input.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

const H4 = 4 * 60 * 60;
const H1 = 60 * 60;
const M5 = 5 * 60;
const NS = 1_000_000_000;
const H4_NS = H4 * NS;
const H1_NS = H1 * NS;
const M5_NS = M5 * NS;

function drive(secondsInOrder) {
    const r = new flox.BarDispatchRecorder();
    for (const s of secondsInOrder) r.addTimeIntervalSeconds(s);
    r.onTrade(1, 100.0, 0.1, 0);
    r.onTrade(1, 101.0, 0.1, H4_NS);
    r.finalize();
    const out = [];
    for (let i = 0; i < r.count(); i++) out.push(r.paramAt(i));
    r.destroy();
    return out;
}

{
    const params = drive([H4, H1, M5]);
    check(params[0] === H4_NS && params[1] === H1_NS && params[2] === M5_NS,
          'coarsest-first registration: tied close fires H4, H1, M5');
}

{
    const params = drive([M5, H1, H4]);
    check(params[0] === M5_NS && params[1] === H1_NS && params[2] === H4_NS,
          'reverse registration: tied close fires M5, H1, H4');
}

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('bar-close ordering parity smoke failed');
}
console.log('\n' + passed + ' check(s) passed');
