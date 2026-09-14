'use strict';
/**
 * node/test/test_portfolio_risk.js — smoke-test the C++-backed portfolio
 * risk aggregator NAPI binding.
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

console.log('=== construction ===');
{
  const agg = new flox.PortfolioRiskAggregator({
    rules: { maxDrawdownPct: 0.20, maxDailyLoss: 10_000 },
    initialEquity: 100_000,
  });
  const snap = agg.snapshot();
  check(snap.killSwitchActive === false, 'fresh aggregator: kill switch off');
  check(snap.totalDailyPnl === 0, 'fresh aggregator: zero daily PnL');
  check(snap.currentEquity === 100_000, 'fresh aggregator: equity = initial');
  check(snap.accountCount === 0, 'fresh aggregator: no accounts');
}

console.log('=== update + snapshot ===');
{
  const agg = new flox.PortfolioRiskAggregator({ initialEquity: 100_000 });
  agg.update('ema', {
    realizedPnl: 120, unrealizedPnl: -30, fees: -5,
    grossExposure: 5000, netExposure: 4500, tradeCount: 12,
  });
  const snap = agg.snapshot();
  check(Math.abs(snap.totalDailyPnl - (120 - 30 - 5)) < 1e-9, 'totalDailyPnl reflects single account');
  check(snap.totalGrossExposure === 5000, 'gross exposure aggregates');
  check(snap.accountCount === 1, 'account count = 1 after update');
}

console.log('=== drawdown breach trips kill switch ===');
{
  const agg = new flox.PortfolioRiskAggregator({
    rules: { maxDrawdownPct: 0.10 },
    initialEquity: 100_000,
  });
  agg.update('s1', { realizedPnl: -20_000 });
  const snap = agg.snapshot();
  check(snap.killSwitchActive === true, 'kill switch trips on 20% drawdown vs 10% cap');
  check(snap.breaches.length >= 1, 'at least one breach recorded');
  const dd = snap.breaches.find(b => b.rule === 'max_drawdown_pct');
  check(dd !== undefined, 'breach rule = max_drawdown_pct');
}

console.log('=== check_order ===');
{
  const agg = new flox.PortfolioRiskAggregator({
    rules: { maxGrossExposure: 10_000 },
  });
  agg.update('s1', { grossExposure: 7_000 });
  const ok = agg.checkOrder('s1', 2_000, 'buy');
  check(ok === null, 'order within cap is allowed');
  const blocked = agg.checkOrder('s1', 5_000, 'buy');
  check(blocked !== null && blocked.rule === 'max_gross_exposure',
        'order over cap is rejected with max_gross_exposure');
}

console.log('=== reset_kill_switch ===');
{
  const agg = new flox.PortfolioRiskAggregator({
    rules: { maxDrawdownPct: 0.05 },
    initialEquity: 100_000,
  });
  agg.update('s1', { realizedPnl: -10_000 });
  check(agg.killSwitchActive() === true, 'kill switch active after breach');
  agg.resetKillSwitch();
  check(agg.killSwitchActive() === false, 'reset_kill_switch clears the flag');
}

console.log('=== gate agrees with snapshot ===');
{
  // One strategy is 100% of its own book by definition. The gate used to
  // refuse every opening order it sent while the snapshot showed nothing.
  const agg = new flox.PortfolioRiskAggregator({
    rules: { maxConcentrationPct: 0.40 },
    initialEquity: 100_000,
  });
  check(agg.checkOrder('solo', 10_000, 'buy') === null,
        'solo strategy can open a position');
  agg.update('solo', { grossExposure: 5_000 });
  check(agg.checkOrder('solo', 10_000, 'buy') === null,
        'solo strategy can add to a position');
  check(agg.snapshot().killSwitchActive === false,
        'solo strategy does not arm the kill switch');
}

{
  // One of two strategies flattens. Counting registered rows rather than
  // contributing ones halted the whole portfolio here.
  const agg = new flox.PortfolioRiskAggregator({
    rules: { maxConcentrationPct: 0.60 },
    initialEquity: 100_000,
  });
  agg.update('a', { grossExposure: 10_000 });
  agg.update('b', { grossExposure: 10_000 });
  agg.update('b', { grossExposure: 0 });
  check(agg.snapshot().killSwitchActive === false,
        'flattening one of two strategies does not halt the portfolio');
}

{
  // Risk-reducing orders pass the exposure cap. Reading an unreported net
  // as zero made every order look like it increased exposure.
  const agg = new flox.PortfolioRiskAggregator({
    rules: { maxGrossExposure: 50_000 },
    initialEquity: 100_000,
  });
  agg.update('a', { grossExposure: 49_000 });
  check(agg.checkOrder('a', 10_000, 'buy') !== null,
        'buy over the gross cap is rejected');
  check(agg.checkOrder('a', 10_000, 'reduce') === null,
        'reduce passes the gross cap');
  check(agg.checkOrder('a', 10_000, 'sell') === null,
        'sell against reported gross passes the gross cap');
}

console.log('=== drawdown needs a capital base ===');
{
  let threw = false;
  try {
    new flox.PortfolioRiskAggregator({ rules: { maxDrawdownPct: 0.20 } });
  } catch (e) {
    threw = true;
  }
  check(threw, 'maxDrawdownPct without initialEquity is refused');

  let ok = true;
  try {
    new flox.PortfolioRiskAggregator({ rules: { maxGrossExposure: 1_000 } });
  } catch (e) {
    ok = false;
  }
  check(ok, 'other rules still construct with no initial equity');
}

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
