'use strict';
/**
 * node/test/test_ns_timestamp_round_trip.js -- the live queue-position
 * estimator and the multi-feed clock carry a wall-clock nanosecond
 * reading without losing any of it.
 *
 * Both classes exist to police time: one ages a queue-position estimate
 * by a confidence half-life, the other decides whether a feed is stale.
 * Both read every ts_ns argument through Napi::Number::Int64Value() and
 * hand it back with a static_cast<double>, and neither includes the
 * BigInt-aware toInt64Ns helper from node/src/data_ops.h, whose comment
 * cites the exact corruption this test pins:
 *
 *   1765615835519000000  ->  1765615835519000064
 *
 * At that magnitude a double steps 256 ns at a time, so a staleness
 * budget can be wrong by up to 512 ns, and a BigInt argument -- the type
 * docs/explanation/javascript-value-boundary.md says every nanosecond
 * argument accepts -- is rejected outright with "A number was expected".
 *
 * Durations are a separate matter and stay Numbers: a staleness, a
 * confidence half-life and a timeout are differences, exact in a double
 * below 104 days. Only clock readings are pinned to BigInt here.
 *
 * Run from repo root:
 *   cd node && node test/test_ns_timestamp_round_trip.js
 */

const fs = require('fs');
const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const NS = 1765615835519000000n;
const SECOND_NS = 1000000000n;
const LATER_NS = NS + SECOND_NS;

function exact(v) {
  return typeof v === 'bigint' ? v : BigInt(v);
}

function accepts(label, fn) {
  try {
    fn();
    check(true, `${label} accepts a BigInt ts`);
    return true;
  } catch (e) {
    check(false, `${label} accepts a BigInt ts (threw ${e.constructor.name}: ${e.message})`);
    return false;
  }
}

// ── LiveQueuePositionEstimator ────────────────────────────────────────

const ORDER_ID = 7;
const SYMBOL = 1;
const BUY = 0;

function placedEstimator(tsNs) {
  const est = new flox.LiveQueuePositionEstimator();
  est.onOrderPlaced(SYMBOL, BUY, 100.0, ORDER_ID, 5.0, 50.0, tsNs);
  return est;
}

console.log('=== LiveQueuePositionEstimator takes a BigInt ts on every method ===');
{
  // onOrderPlaced is both the argument under test and the fixture for
  // the rest, so it is checked on its own first.
  accepts('onOrderPlaced', () => placedEstimator(NS));

  const methods = [
    ['onTrade', est => est.onTrade(SYMBOL, 100.0, 1.0, LATER_NS)],
    ['onTradeWithFlag', est => est.onTradeWithFlag(SYMBOL, 100.0, 1.0, LATER_NS, false)],
    ['onLevelUpdate', est => est.onLevelUpdate(SYMBOL, BUY, 100.0, 40.0, LATER_NS)],
    ['onOrderFilled', est => est.onOrderFilled(ORDER_ID, 1.0, LATER_NS)],
    ['onOrderCancelled', est => est.onOrderCancelled(ORDER_ID, LATER_NS)],
    ['snapshot', est => est.snapshot(ORDER_ID, LATER_NS)],
  ];
  for (const [name, call] of methods) {
    let est;
    try { est = placedEstimator(Number(NS)); } catch (_) { est = null; }
    if (est === null) { check(false, `${name}: fixture order could not be placed`); continue; }
    accepts(name, () => call(est));
  }
}

console.log('\n=== The estimator gives a reading past 2^53 back unchanged ===');
{
  let snap = null;
  try {
    snap = placedEstimator(NS).snapshot(ORDER_ID, NS);
    check(snap !== null, 'snapshot() found the placed order');
  } catch (e) {
    check(false, `place and read back a BigInt ts (threw ${e.constructor.name}: ${e.message})`);
  }
  if (snap !== null) {
    check(typeof snap.lastUpdateNs === 'bigint',
          `snapshot().lastUpdateNs is a bigint (got ${typeof snap.lastUpdateNs})`);
    check(exact(snap.lastUpdateNs) === NS,
          `onOrderPlaced ts survives (got ${exact(snap.lastUpdateNs)}, want ${NS})`);
  }
}

// Every method that moves the estimator's clock has to move it to the
// exact value it was handed, not to the nearest double.
for (const [name, call] of [
  ['onTrade', est => est.onTrade(SYMBOL, 100.0, 1.0, LATER_NS)],
  ['onTradeWithFlag', est => est.onTradeWithFlag(SYMBOL, 100.0, 1.0, LATER_NS, false)],
  ['onLevelUpdate', est => est.onLevelUpdate(SYMBOL, BUY, 100.0, 40.0, LATER_NS)],
  ['onOrderFilled', est => est.onOrderFilled(ORDER_ID, 1.0, LATER_NS)],
]) {
  let snap = null;
  try {
    const est = placedEstimator(NS);
    call(est);
    snap = est.snapshot(ORDER_ID, LATER_NS);
  } catch (e) {
    check(false, `${name} ts survives (threw ${e.constructor.name}: ${e.message})`);
    continue;
  }
  check(snap !== null, `${name}: snapshot() still finds the order`);
  if (snap === null) { continue; }
  check(exact(snap.lastUpdateNs) === LATER_NS,
        `${name} ts survives (got ${exact(snap.lastUpdateNs)}, want ${LATER_NS})`);
}

console.log('\n=== onOrderCancelled takes a BigInt ts and retires the order ===');
{
  let snap;
  try {
    const est = placedEstimator(NS);
    est.onOrderCancelled(ORDER_ID, LATER_NS);
    snap = est.snapshot(ORDER_ID, LATER_NS);
    check(snap === null, `a cancelled order is gone from the estimator (got ${JSON.stringify(snap)})`);
  } catch (e) {
    check(false, `onOrderCancelled with a BigInt ts (threw ${e.constructor.name}: ${e.message})`);
  }
}

console.log('\n=== A Number ts still works ===');
{
  const est = placedEstimator(60000000000);
  const snap = est.snapshot(ORDER_ID, 60000000000);
  check(snap !== null, 'a Number ts still places an order');
  if (snap !== null) {
    check(exact(snap.lastUpdateNs) === 60000000000n,
          `a Number ts under 2^53 survives (got ${exact(snap.lastUpdateNs)})`);
  }
}

console.log('\n=== Why the BigInt path is needed at all ===');
{
  // The caller cannot reach NS through a Number: the literal is already
  // the nearest double by the time the binding sees it. This is the
  // corruption node/src/data_ops.h's comment names, reproduced here so
  // the requirement above reads as a consequence rather than a taste.
  check(BigInt(Number(NS)) === 1765615835519000064n,
        `${NS} written as a Number is already ${BigInt(Number(NS))}`);
  const est = placedEstimator(Number(NS));
  const snap = est.snapshot(ORDER_ID, Number(NS));
  check(snap !== null && exact(snap.lastUpdateNs) === BigInt(Number(NS)),
        'a Number ts comes back as the double it was, not as the reading the caller meant');
}

// ── MultiFeedClock ────────────────────────────────────────────────────

const BTC = 1;
const ETH = 2;

console.log('\n=== MultiFeedClock.tick takes a BigInt ts ===');
{
  const clock = new flox.MultiFeedClock({ symbols: [BTC, ETH], policy: 'FireOnAny' });
  accepts('MultiFeedClock.tick', () => clock.tick(NS, BTC));
}

console.log('\n=== The clock gives a reading past 2^53 back unchanged ===');
{
  const clock = new flox.MultiFeedClock({ symbols: [BTC, ETH], policy: 'FireOnAny' });
  let snap;
  try {
    snap = clock.tick(NS, BTC);
  } catch (e) {
    check(false, `tick() with a BigInt ts (threw ${e.constructor.name}: ${e.message})`);
    snap = null;
  }
  if (snap !== null) {
    check(typeof snap.lastTsNs[BTC] === 'bigint',
          `lastTsNs[BTC] is a bigint (got ${typeof snap.lastTsNs[BTC]})`);
    check(exact(snap.lastTsNs[BTC]) === NS,
          `lastTsNs[BTC] survives (got ${exact(snap.lastTsNs[BTC])}, want ${NS})`);
  }
}

console.log('\n=== Staleness stays an exact difference of two exact readings ===');
{
  const clock = new flox.MultiFeedClock({ symbols: [BTC, ETH], policy: 'WaitForAll' });
  let snap;
  try {
    clock.tick(NS, BTC);
    snap = clock.tick(NS + 100000000n, ETH);
  } catch (e) {
    check(false, `two BigInt ticks (threw ${e.constructor.name}: ${e.message})`);
    snap = null;
  }
  if (snap !== null) {
    // stalenessNs is a duration -- its JS type is not pinned here, only
    // that it is the exact difference rather than a double subtraction.
    check(exact(snap.stalenessNs[BTC]) === 100000000n,
          `BTC staleness is exactly 100 ms (got ${exact(snap.stalenessNs[BTC])})`);
    check(exact(snap.stalenessNs[ETH]) === 0n,
          `ETH staleness is exactly 0 (got ${exact(snap.stalenessNs[ETH])})`);
  }
}

console.log('\n=== A Number ts still ticks the clock ===');
{
  const clock = new flox.MultiFeedClock({ symbols: [BTC, ETH], policy: 'FireOnAny' });
  const snap = clock.tick(Number(SECOND_NS), BTC);
  check(snap.fired === true, 'a Number ts still fires FireOnAny');
  check(exact(snap.lastTsNs[BTC]) === SECOND_NS,
        `a Number ts under 2^53 survives (got ${exact(snap.lastTsNs[BTC])})`);
}

// ── index.d.ts agrees with what the addon accepts and returns ─────────

console.log('\n=== index.d.ts types the readings bigint on both sides ===');
{
  const dts = fs.readFileSync(path.join(__dirname, '..', 'index.d.ts'), 'utf8');

  function block(header) {
    const m = dts.match(new RegExp(`${header}\\s*\\{([\\s\\S]*?)\\n\\}`));
    return m ? m[1] : null;
  }

  const snapshotBody = block('export interface LiveQueueSnapshot');
  check(snapshotBody !== null, 'index.d.ts declares LiveQueueSnapshot');
  if (snapshotBody !== null) {
    const declared = snapshotBody.match(/^\s*lastUpdateNs:\s*([^;]+);/m);
    check(declared !== null, 'LiveQueueSnapshot.lastUpdateNs is declared');
    if (declared) {
      check(declared[1].trim() === 'bigint',
            `LiveQueueSnapshot.lastUpdateNs is typed bigint (declared "${declared[1].trim()}")`);
    }
  }

  const clockBody = block('export interface FeedClockSnapshot');
  check(clockBody !== null, 'index.d.ts declares FeedClockSnapshot');
  if (clockBody !== null) {
    const declared = clockBody.match(/^\s*lastTsNs:\s*([^;]+);/m);
    check(declared !== null, 'FeedClockSnapshot.lastTsNs is declared');
    if (declared) {
      check(/bigint/.test(declared[1]),
            `FeedClockSnapshot.lastTsNs maps to bigint (declared "${declared[1].trim()}")`);
    }
  }

  // Write side: every ts argument admits both types.
  const estimatorBody = block('export class LiveQueuePositionEstimator');
  check(estimatorBody !== null, 'index.d.ts declares LiveQueuePositionEstimator');
  if (estimatorBody !== null) {
    const tsParams = estimatorBody.match(/\b(tsNs|nowNs)\??:\s*[^,)]+/g) || [];
    check(tsParams.length > 0, 'LiveQueuePositionEstimator declares ts parameters');
    for (const param of tsParams) {
      check(/bigint/.test(param),
            `LiveQueuePositionEstimator "${param.trim()}" admits bigint`);
    }
  }

  const clockClass = block('export class MultiFeedClock');
  check(clockClass !== null, 'index.d.ts declares MultiFeedClock');
  if (clockClass !== null) {
    const tick = clockClass.match(/tick\(([^)]*)\)/);
    check(tick !== null, 'MultiFeedClock declares tick()');
    if (tick) {
      check(/bigint/.test(tick[1]),
            `MultiFeedClock.tick admits a bigint ts (declared "tick(${tick[1].trim()})")`);
    }
  }
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
