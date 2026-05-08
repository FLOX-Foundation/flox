// Smoke test for the latency models exposed on the flox global.
// Runs under the flox QuickJS host. Asserts construction, sampling,
// reproducibility under reset(seed), and validation of bad inputs.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

// ── ConstantLatency ────────────────────────────────────────────────
{
    var c = new flox.ConstantLatency({ feedNs: 100, orderNs: 200, fillNs: 300 });
    check(c.feedDelay() === 100, 'constant feed_delay returns configured value');
    check(c.orderDelay() === 200, 'constant order_delay returns configured value');
    check(c.fillDelay() === 300, 'constant fill_delay returns configured value');
    var s = c.sample();
    check(s.feedNs === 100 && s.orderNs === 200 && s.fillNs === 300,
          'sample() returns LatencySample with all three');
    c.destroy();
}

// ── GaussianLatency reproducibility ────────────────────────────────
{
    var g = new flox.GaussianLatency({
        feedMeanNs: 1000, feedStddevNs: 200, seed: 42,
    });
    var a = g.feedDelay();
    g.reset(42);
    var b = g.feedDelay();
    check(a === b, 'gaussian sequence is reproducible under reset(seed)');
    var c = new flox.GaussianLatency({ feedMeanNs: 1500, feedStddevNs: 0, seed: 0 });
    check(c.feedDelay() === 1500, 'stddev=0 collapses to mean');
    g.destroy();
    c.destroy();
}

// ── ExponentialLatency non-negativity ──────────────────────────────
{
    var e = new flox.ExponentialLatency({ feedMeanNs: 500, seed: 7 });
    var allNonNeg = true;
    for (var i = 0; i < 50; i++) {
        if (e.feedDelay() < 0) { allNonNeg = false; break; }
    }
    check(allNonNeg, 'exponential samples are non-negative');
    e.destroy();
}

// ── EmpiricalLatency draws subset ──────────────────────────────────
{
    var emp = new flox.EmpiricalLatency({ feedSamples: [10, 20, 30], seed: 0 });
    var allowed = { 10: 1, 20: 1, 30: 1 };
    var allInSet = true;
    for (var i = 0; i < 30; i++) {
        if (!allowed[emp.feedDelay()]) { allInSet = false; break; }
    }
    check(allInSet, 'empirical draws are subset of input samples');
    check(emp.orderDelay() === 0, 'empirical with no order_samples returns 0');
    emp.destroy();
}

// ── Validation ─────────────────────────────────────────────────────
{
    var threw = false;
    try { new flox.ConstantLatency({ feedNs: -1 }); }
    catch (e) { threw = true; }
    check(threw, 'negative feed_ns rejected');
    var threw2 = false;
    try { new flox.EmpiricalLatency({}); }
    catch (e) { threw2 = true; }
    check(threw2, 'all-empty empirical rejected');
}

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('latency smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
