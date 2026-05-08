// Smoke test for execution algos exposed on the flox global.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

// TWAP
{
    var algo = new flox.TWAPExecutor({
        targetQty: 100, side: 'buy', symbol: 1,
        durationNs: 10000, sliceCount: 5, startTimeNs: 0,
    });
    var c1 = algo.step(0);
    check(c1.length === 1, 'TWAP first step emits one slice');
    check(Math.abs(c1[0].qty - 20) < 1e-9, 'TWAP slice qty = 20');
    algo.step(2000);
    algo.step(10000);
    check(algo.isDone(), 'TWAP completes by end of duration');
    algo.destroy();
}

// VWAP
{
    var algo = new flox.VWAPExecutor({
        targetQty: 100, side: 'buy', symbol: 1,
        volumeCurve: [[1000, 200], [2000, 300], [3000, 500]],
    });
    var c1 = algo.step(2500);
    check(c1.length === 2, 'VWAP emits one slice per elapsed bar');
    algo.step(3000);
    check(algo.isDone(), 'VWAP completes after curve exhausted');
    algo.destroy();
}

// Iceberg
{
    var algo = new flox.IcebergExecutor({
        targetQty: 50, side: 'buy', symbol: 1,
        type: 'limit', limitPrice: 100, visibleQty: 10,
    });
    var c1 = algo.step(0);
    check(c1.length === 1, 'Iceberg shows one visible slice');
    check(algo.step(1).length === 0, 'Iceberg does not refill while outstanding');
    algo.reportFill(10);
    check(algo.step(2).length === 1, 'Iceberg refills once child filled');
    algo.destroy();
}

// POV
{
    var algo = new flox.POVExecutor({
        targetQty: 100, side: 'buy', symbol: 1,
        participationRate: 0.10, minSliceQty: 1,
    });
    check(algo.step(0).length === 0, 'POV does not slice without observed volume');
    algo.observeVolume(50);
    var c = algo.step(1);
    check(c.length === 1 && Math.abs(c[0].qty - 5) < 1e-9, 'POV slices 10% of observed');
    algo.destroy();
}

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('execution algos smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
