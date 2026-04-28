'use strict';
/**
 * node/test/test_bindings.js — smoke-test all major Node.js binding surfaces.
 *
 * Run from repo root:
 *   cd node && node test/test_bindings.js
 */

const path = require('path');
const fs   = require('fs');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;

function check(condition, msg) {
  if (condition) {
    console.log(`  ok  ${msg}`);
    passed++;
  } else {
    console.error(`  FAIL  ${msg}`);
    failed++;
  }
}

function approx(a, b, eps = 1e-6) {
  return isFinite(a) && isFinite(b) && Math.abs(a - b) < eps;
}

// ── Constants ─────────────────────────────────────────────────────────

console.log('=== Constants ===');
check(flox.POSITION_FIFO === 0, 'POSITION_FIFO === 0');
check(flox.POSITION_AVG_COST === 1, 'POSITION_AVG_COST === 1');
check(flox.SLIPPAGE_NONE === 0, 'SLIPPAGE_NONE === 0');
check(flox.SLIPPAGE_FIXED_TICKS === 1, 'SLIPPAGE_FIXED_TICKS === 1');
check(flox.SLIPPAGE_FIXED_BPS === 2, 'SLIPPAGE_FIXED_BPS === 2');
check(flox.SLIPPAGE_VOLUME_IMPACT === 3, 'SLIPPAGE_VOLUME_IMPACT === 3');
check(flox.QUEUE_NONE === 0, 'QUEUE_NONE === 0');
check(flox.QUEUE_TOB === 1, 'QUEUE_TOB === 1');
check(flox.QUEUE_FULL === 2, 'QUEUE_FULL === 2');

// ── Streaming indicators ──────────────────────────────────────────────

console.log('=== Streaming indicators ===');

const sma = new flox.SMA(3);
sma.update(1.0); sma.update(2.0); sma.update(3.0);
check(approx(sma.value, 2.0), 'SMA(3) of [1,2,3] == 2.0');
sma.update(4.0);
check(approx(sma.value, 3.0), 'SMA(3) shifts to [2,3,4] == 3.0');
check(sma.ready === true, 'SMA.ready === true');

const ema = new flox.EMA(3);
for (let i = 1; i <= 10; i++) ema.update(i);
check(ema.value > 0, 'EMA.value > 0 after 10 updates');
check(ema.ready === true, 'EMA.ready === true');

const rsi = new flox.RSI(14);
for (let i = 1; i <= 20; i++) rsi.update(i);
check(rsi.value >= 0 && rsi.value <= 100, `RSI in [0,100], got ${rsi.value.toFixed(2)}`);

const atr = new flox.ATR(14);
for (let i = 0; i < 20; i++) atr.update(100 + i, 99 + i, 100 + i);
check(atr.value > 0, 'ATR > 0');

// ── Batch indicators ──────────────────────────────────────────────────

console.log('=== Batch indicators ===');

const prices = new Float64Array([1, 2, 3, 4, 5, 6, 7]);
const bSma = flox.sma(prices, 3);
check(approx(bSma[2], 2.0), 'batch sma[2] == 2.0');
check(approx(bSma[6], 6.0), 'batch sma[6] == 6.0');

const bEma = flox.ema(prices, 3);
check(bEma.length === prices.length, 'batch ema length matches input');
check(bEma[6] > bEma[2], 'batch ema increases for ascending input');

// ── Targets (forward-looking labels) ──────────────────────────────────

console.log('=== Targets ===');

const tClose = new Float64Array([100.0, 101.0, 99.0, 105.0, 110.0]);
const fr = flox.targets.future_return(tClose, 2);
check(approx(fr[0], 99.0 / 100.0 - 1.0), 'targets.future_return[0]');
check(approx(fr[2], 110.0 / 99.0 - 1.0), 'targets.future_return[2]');
check(Number.isNaN(fr[3]) && Number.isNaN(fr[4]), 'targets.future_return tail NaN');

const constClose = new Float64Array(20).fill(100.0);
const vol = flox.targets.future_ctc_volatility(constClose, 5);
check(approx(vol[0], 0.0), 'targets.future_ctc_volatility const-series == 0');
check(Number.isNaN(vol[19]), 'targets.future_ctc_volatility tail NaN');

const linear = new Float64Array(20);
for (let i = 0; i < 20; ++i) linear[i] = 100.0 + 0.5 * i;
const sl = flox.targets.future_linear_slope(linear, 4);
check(approx(sl[0], 0.5), 'targets.future_linear_slope linear-series == 0.5');
check(Number.isNaN(sl[19]), 'targets.future_linear_slope tail NaN');

// ── IndicatorGraph (batch) ────────────────────────────────────────────

console.log('=== IndicatorGraph (batch) ===');

const ramp = new Float64Array(50);
for (let i = 0; i < 50; ++i) ramp[i] = i;

const g = new flox.IndicatorGraph();
g.setBars(0, ramp);

g.addNode('ema5', [], (graph, sym) => flox.ema(graph.close(sym), 5));
g.addNode('sma5', [], (graph, sym) => flox.sma(graph.close(sym), 5));
g.addNode('diff', ['ema5', 'sma5'], (graph, sym) => {
  const a = graph.get(sym, 'ema5');
  const b = graph.get(sym, 'sma5');
  const out = new Float64Array(a.length);
  for (let i = 0; i < a.length; ++i) out[i] = a[i] - b[i];
  return out;
});

const ema5 = g.require(0, 'ema5');
const diff = g.require(0, 'diff');
check(ema5.length === 50, 'graph require returns full-length array');
check(diff.length === 50, 'graph dependent node has same length');
check(g.get(0, 'sma5') !== null, 'graph get returns cached node array');
check(g.get(0, 'never_added') === null, 'graph get on missing node returns null');

let threw = false;
try { g.require(0, 'missing'); } catch (e) { threw = true; }
check(threw, 'graph require on unknown node throws');

// ── PositionTracker ───────────────────────────────────────────────────

console.log('=== PositionTracker ===');

const pt = new flox.PositionTracker(flox.POSITION_FIFO);
pt.onFill(0, 'buy', 50000.0, 1.0);
check(approx(pt.position(0), 1.0), 'position == 1.0 after buy');
check(approx(pt.avgEntryPrice(0), 50000.0), 'avgEntryPrice == 50000');
check(approx(pt.realizedPnl(0), 0.0), 'realizedPnl == 0 before close');

pt.onFill(0, 'sell', 51000.0, 1.0);
check(approx(pt.position(0), 0.0), 'position == 0 after sell');
check(pt.realizedPnl(0) > 0, 'realizedPnl > 0 on winning trade');
check(approx(pt.realizedPnl(0), 1000.0), 'realizedPnl == 1000');
check(approx(pt.totalRealizedPnl(), 1000.0), 'totalRealizedPnl == 1000');

// Multi-symbol
const pt2 = new flox.PositionTracker(flox.POSITION_FIFO);
pt2.onFill(0, 'buy', 100.0, 2.0);
pt2.onFill(1, 'sell', 200.0, 3.0);
check(approx(pt2.position(0), 2.0), 'symbol 0 position == 2.0');
check(approx(pt2.position(1), -3.0), 'symbol 1 position == -3.0');

// ── OrderTracker ──────────────────────────────────────────────────────

console.log('=== OrderTracker ===');

const ot = new flox.OrderTracker();
check(ot.onSubmitted(1001, 0, 'buy', 50000.0, 1.0) === true, 'onSubmitted returns true');
check(ot.isActive(1001) === true, 'order 1001 is active');
check(ot.activeCount === 1, 'activeCount === 1');
check(ot.totalCount === 1, 'totalCount === 1');

ot.onSubmitted(1002, 0, 'sell', 51000.0, 1.0);
check(ot.activeCount === 2, 'activeCount === 2 after second order');

check(ot.onFilled(1001, 1.0) === true, 'onFilled returns true');
check(ot.isActive(1001) === false, 'order 1001 inactive after fill');
check(ot.activeCount === 1, 'activeCount === 1 after fill');

check(ot.onCanceled(1002) === true, 'onCanceled returns true');
check(ot.isActive(1002) === false, 'order 1002 inactive after cancel');
check(ot.activeCount === 0, 'activeCount === 0');
check(ot.totalCount === 2, 'totalCount === 2');

// ── Engine (data loading) ─────────────────────────────────────────────

console.log('=== Engine ===');

const csvPath = path.join(__dirname, '..', '..', 'docs', 'examples', 'data', 'btcusdt_1m.csv');
if (fs.existsSync(csvPath)) {
  const engine = new flox.Engine();
  engine.loadCsv(csvPath);
  const n = engine.barCount();
  check(n > 1000, `barCount ${n} > 1000`);
  const opens  = engine.open();
  const closes = engine.close();
  const highs  = engine.high();
  const lows   = engine.low();
  check(opens.length  === n, 'opens.length  == barCount');
  check(closes.length === n, 'closes.length == barCount');
  check(opens[0] > 0, 'first open price > 0');
  check(highs.every((h, i) => h >= lows[i]), 'high >= low for every bar');
  console.log(`  loaded ${n} bars, price ${Math.min(...closes).toFixed(0)}–${Math.max(...closes).toFixed(0)}`);
} else {
  console.log(`  skip Engine (CSV not found: ${csvPath})`);
}

// ── Summary ───────────────────────────────────────────────────────────

console.log(`\n${passed} passed, ${failed} failed`);
if (failed > 0) process.exit(1);
