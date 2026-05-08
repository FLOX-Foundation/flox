// Smoke test for flox.PortfolioRiskAggregator exposed on the flox global.
// Builds a small multi-strategy view and exercises the kill-switch path.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

// Construction.
{
    var agg = new flox.PortfolioRiskAggregator({
        rules: { maxDrawdownPct: 0.20, maxDailyLoss: 10000 },
        initialEquity: 100000,
    });
    var snap = agg.snapshot();
    check(snap.killSwitchActive === false, 'fresh: kill switch off');
    check(snap.totalDailyPnl === 0, 'fresh: zero daily PnL');
    check(snap.currentEquity === 100000, 'fresh: equity = initial');
    check(snap.accountCount === 0, 'fresh: no accounts');
    agg.destroy();
}

// Update + snapshot.
{
    var agg = new flox.PortfolioRiskAggregator({ initialEquity: 100000 });
    agg.update('ema', {
        realizedPnl: 120, unrealizedPnl: -30, fees: -5,
        grossExposure: 5000, netExposure: 4500, tradeCount: 12,
    });
    var snap = agg.snapshot();
    check(Math.abs(snap.totalDailyPnl - 85) < 1e-9,
          'totalDailyPnl reflects single account');
    check(snap.totalGrossExposure === 5000, 'gross exposure aggregates');
    check(snap.accountCount === 1, 'account count = 1 after update');
    agg.destroy();
}

// Drawdown breach.
{
    var agg = new flox.PortfolioRiskAggregator({
        rules: { maxDrawdownPct: 0.10 },
        initialEquity: 100000,
    });
    agg.update('s1', { realizedPnl: -20000 });
    var snap = agg.snapshot();
    check(snap.killSwitchActive === true,
          'kill switch trips on 20% drawdown vs 10% cap');
    check(snap.breaches.length >= 1, 'breach recorded');
    agg.destroy();
}

// Pre-trade check.
{
    var agg = new flox.PortfolioRiskAggregator({
        rules: { maxGrossExposure: 10000 },
    });
    agg.update('s1', { grossExposure: 7000 });
    var ok = agg.checkOrder('s1', 2000, 'buy');
    check(ok === null, 'order within cap allowed');
    var blocked = agg.checkOrder('s1', 5000, 'buy');
    check(blocked !== null && blocked.rule === 'max_gross_exposure',
          'order over cap rejected');
    agg.destroy();
}

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('portfolio risk smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
