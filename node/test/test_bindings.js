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
