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
 * The same defect sits in node/src/hooks.h, which builds every hook
 * payload: orderToJs and tradeToJs still hand `createdAtNs` and
 * `exchangeTsNs` over as Napi::Number, so every execution-listener
 * event, every Executor.submit and every market-data recorder trade
 * carries a quantised reading -- while the trade a strategy is handed,
 * built 400 lines away in strategy.h, has been a BigInt all along.
 *
 * Durations are a separate matter and stay Numbers: a staleness, a
 * confidence half-life and a timeout are differences, exact in a double
 * below 104 days. So are prices and sizes. Only clock readings are
 * pinned to BigInt here.
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

// Values printed in failure text may be BigInts, which String() handles
// and JSON.stringify refuses.
function describeValue(v) {
  return typeof v === 'string' ? JSON.stringify(v) : String(v);
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

// ── Order and trade events ────────────────────────────────────────────
//
// node/src/hooks.h builds the JS objects every hook payload is made of.
// The bar and context builders in strategy.h were moved onto BigInt;
// orderToJs (:112-113) and tradeToJs (:124) were not, and still emit
// `createdAtNs` / `exchangeTsNs` through
// Napi::Number::New(env, static_cast<double>(...)). Those are absolute
// clock readings, which the boundary doc puts on the BigInt side -- "on
// an order event: exchangeTsNs, submittedAtNs, ..." -- and which the
// trade handed to a strategy already honours (strategy.h sets
// TradeData.timestampNs with Napi::BigInt). Every execution-listener
// event, every Executor.submit, and every market-data recorder trade
// goes through these two functions, so the whole hook payload surface
// still quantises its timestamps to 256 ns.

const BAR_STARTS = [NS, NS + SECOND_NS];
const BAR_ENDS = [NS + SECOND_NS - 1n, NS + 2n * SECOND_NS - 1n];

// The clock the runner stamps an order with is the bar boundary it was
// submitted on, so the exact reading is one of the ends fed in -- and
// they are a second apart, four million times the 256 ns a double can
// resolve here, so no double image of one can be mistaken for another.
function isOneOfTheBarEnds(v) {
  return BAR_ENDS.some(e => e === exact(v));
}

console.log('\n=== An order reaching an execution listener carries BigInt readings ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.BacktestRunner(reg, 0.0, 10000.0);
  const orders = [];
  runner.addExecutionListener({
    onSubmitted(o) { orders.push(['onSubmitted', o]); },
    onAccepted(o) { orders.push(['onAccepted', o]); },
    onFilled(o) { orders.push(['onFilled', o]); },
  });
  let bar = 0;
  runner.setStrategy({
    symbols: [sym],
    onBar(_ctx, _b, emit) { bar++; if (bar === 1) { emit.marketBuy(1.0, Number(sym)); } },
  });
  runner.runBars(
    new BigInt64Array(BAR_STARTS), new BigInt64Array(BAR_ENDS),
    new Float64Array([100.0, 100.5]), new Float64Array([101.0, 101.5]),
    new Float64Array([99.0, 99.5]), new Float64Array([100.5, 101.0]),
    new Float64Array([10.0, 10.0]), 'BTC');

  check(orders.length >= 1, `the listener saw at least one order event (got ${orders.length})`);
  for (const [event, order] of orders) {
    check(typeof order.createdAtNs === 'bigint',
          `${event} order.createdAtNs is a bigint (got ${typeof order.createdAtNs})`);
    check(isOneOfTheBarEnds(order.createdAtNs),
          `${event} order.createdAtNs is one of the bar ends fed in ` +
          `(got ${exact(order.createdAtNs)}, want ${BAR_ENDS.join(' or ')})`);
    check(typeof order.exchangeTsNs === 'bigint',
          `${event} order.exchangeTsNs is a bigint (got ${typeof order.exchangeTsNs})`);
    // Prices and sizes are not clock readings and stay Numbers.
    check(typeof order.price === 'number' && typeof order.quantity === 'number',
          `${event} order keeps price and quantity as numbers`);
  }
}

console.log('\n=== An order reaching a binding-supplied executor carries the same types ===');
{
  // Executor.submit goes through the same orderToJs. The live runner
  // does not stamp created_at_ns, so only the type is pinned here; the
  // value is pinned on the listener path above.
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.Runner(reg, () => {}, false);
  const submitted = [];
  runner.setExecutor({
    submit(order) { submitted.push(order); },
    cancel() {},
    capabilities() { return {}; },
  });
  let fired = false;
  runner.addStrategy({
    symbols: [sym],
    onTrade(_ctx, _t, emit) {
      if (fired) { return; }
      fired = true;
      emit.marketBuy(Number(sym), 1.0);
    },
  });
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, NS);
  runner.stop();

  check(submitted.length === 1, `the executor saw one order (got ${submitted.length})`);
  if (submitted.length === 1) {
    check(typeof submitted[0].createdAtNs === 'bigint',
          `Executor.submit order.createdAtNs is a bigint (got ${typeof submitted[0].createdAtNs})`);
    check(typeof submitted[0].exchangeTsNs === 'bigint',
          `Executor.submit order.exchangeTsNs is a bigint (got ${typeof submitted[0].exchangeTsNs})`);
  }
}

console.log('\n=== A trade reaching a market-data recorder keeps its reading ===');

// One buy and one sell, a single nanosecond apart. The gap matters: at
// this magnitude a double steps 256 ns, so two readings one nanosecond
// apart are the same number and different BigInts. Anything that can
// tell them apart went through the exact path.
const BUY_NS = NS;
const SELL_NS = NS + 1n;

function recordedTrades() {
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.Runner(reg, () => {}, false);
  const trades = [];
  runner.setMarketDataRecorder({
    onStart() {}, onStop() {},
    onTrade(trade) { trades.push(trade); },
    onBookUpdate() {},
  });
  runner.addStrategy({ symbols: [sym], onTrade() {} });
  runner.start();
  runner.onTrade(Number(sym), 100, 2.5, true, BUY_NS);
  runner.onTrade(Number(sym), 101, 1.5, false, SELL_NS);
  runner.stop();
  return trades;
}

{
  const trades = recordedTrades();
  check(trades.length === 2, `the recorder saw both trades (got ${trades.length})`);
  if (trades.length === 2) {
    check(typeof trades[0].timestampNs === 'bigint',
          `recorder trade.timestampNs is a bigint (got ${typeof trades[0].timestampNs})`);
    check(exact(trades[0].timestampNs) === BUY_NS,
          `recorder trade.timestampNs round-trips exactly ` +
          `(got ${exact(trades[0].timestampNs)}, want ${BUY_NS})`);
    check(exact(trades[1].timestampNs) === SELL_NS,
          `a reading one nanosecond later stays one nanosecond later ` +
          `(got ${exact(trades[1].timestampNs)}, want ${SELL_NS})`);
    check(typeof trades[0].price === 'number',
          `recorder trade keeps price as a number (got ${typeof trades[0].price})`);
    check(typeof trades[0].qty === 'number' && Math.abs(trades[0].qty - 2.5) < 1e-9,
          `recorder trade.qty is the quantity fed in (got ${describeValue(trades[0].qty)})`);
  }
}

console.log('\n=== The recorder trade mirrors isBuy into side ===');
{
  // side is a second spelling of isBuy, not an independent field: a
  // recorder that keys off one and a strategy that keys off the other
  // have to agree, or a tape records the wrong aggressor.
  const trades = recordedTrades();
  check(trades.length === 2, `the recorder saw both trades (got ${trades.length})`);
  if (trades.length === 2) {
    const [buy, sell] = trades;
    check(buy.isBuy === true, `the buy arrives with isBuy true (got ${describeValue(buy.isBuy)})`);
    check(buy.side === 'buy', `the buy arrives with side "buy" (got ${describeValue(buy.side)})`);
    check(sell.isBuy === false, `the sell arrives with isBuy false (got ${describeValue(sell.isBuy)})`);
    check(sell.side === 'sell', `the sell arrives with side "sell" (got ${describeValue(sell.side)})`);
    for (const trade of trades) {
      check(trade.side === (trade.isBuy ? 'buy' : 'sell'),
            `side agrees with isBuy (isBuy ${describeValue(trade.isBuy)}, ` +
            `side ${describeValue(trade.side)})`);
    }
  }
}

console.log('\n=== The recorder trade keeps its deprecated aliases for one release ===');
{
  // A MarketDataRecorderHook used to be handed `quantity` and
  // `exchangeTsNs` where index.d.ts declared `qty` and `timestampNs`.
  // The declared names are what the addon delivers now; the old two stay
  // alongside them for one release so a recorder written against the
  // shipped behaviour keeps working -- and `exchangeTsNs` stays a
  // BigInt, because a reading that never fit in a double does not fit in
  // one under its old name either.
  const trades = recordedTrades();
  check(trades.length === 2, `the recorder saw both trades (got ${trades.length})`);
  if (trades.length === 2) {
    const trade = trades[0];
    const keys = Object.keys(trade).join(', ');

    check(trade.quantity !== undefined,
          `the trade still carries the deprecated quantity (keys were ${keys})`);
    check(typeof trade.quantity === 'number',
          `quantity is a number (got ${typeof trade.quantity})`);
    check(trade.quantity === trade.qty,
          `quantity equals qty (got ${describeValue(trade.quantity)} vs ${describeValue(trade.qty)})`);

    check(trade.exchangeTsNs !== undefined,
          `the trade still carries the deprecated exchangeTsNs (keys were ${keys})`);
    check(typeof trade.exchangeTsNs === 'bigint',
          `exchangeTsNs is a bigint like timestampNs (got ${typeof trade.exchangeTsNs})`);
    check(trade.exchangeTsNs === trade.timestampNs,
          `exchangeTsNs equals timestampNs ` +
          `(got ${describeValue(trade.exchangeTsNs)} vs ${describeValue(trade.timestampNs)})`);
    // exact() is only meaningful once the alias is there at all; guard
    // it so a dropped alias reports every check rather than throwing out
    // of BigInt(undefined) halfway down the file.
    if (trade.exchangeTsNs !== undefined && trades[1].exchangeTsNs !== undefined) {
      check(exact(trade.exchangeTsNs) === BUY_NS,
            `exchangeTsNs round-trips exactly (got ${exact(trade.exchangeTsNs)}, want ${BUY_NS})`);
      check(exact(trades[1].exchangeTsNs) === SELL_NS,
            `the alias resolves one nanosecond too ` +
            `(got ${exact(trades[1].exchangeTsNs)}, want ${SELL_NS})`);
    }
  }
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

  // The hook payload surface: node/src/hooks.h's orderToJs and
  // tradeToJs feed these two.
  const orderBody = block('export interface Order');
  check(orderBody !== null, 'index.d.ts declares interface Order');
  if (orderBody !== null) {
    for (const field of ['createdAtNs', 'exchangeTsNs']) {
      const declared = orderBody.match(new RegExp(`^\\s*${field}:\\s*([^;]+);`, 'm'));
      check(declared !== null, `Order.${field} is declared`);
      if (declared) {
        check(declared[1].trim() === 'bigint',
              `Order.${field} is typed bigint (declared "${declared[1].trim()}")`);
      }
    }
    check(/^\s*price:\s*number;/m.test(orderBody),
          'Order.price stays a number -- a price is not a clock reading');
  }

  const tradeBody = block('export interface TradeData');
  check(tradeBody !== null, 'index.d.ts declares interface TradeData');
  if (tradeBody !== null) {
    const declared = tradeBody.match(/^\s*timestampNs:\s*([^;]+);/m);
    check(declared !== null, 'TradeData.timestampNs is declared');
    if (declared) {
      check(declared[1].trim() === 'bigint',
            `TradeData.timestampNs is typed bigint (declared "${declared[1].trim()}")`);
    }

    // The two aliases the addon still delivers are declared optional,
    // typed like the fields they mirror, and marked deprecated so an
    // editor steers a new caller to the real name.
    for (const [alias, type] of [['quantity', 'number'], ['exchangeTsNs', 'bigint']]) {
      const aliasDecl = tradeBody.match(new RegExp(`^\\s*${alias}\\?:\\s*([^;]+);`, 'm'));
      check(aliasDecl !== null, `TradeData.${alias} is declared optional`);
      if (aliasDecl) {
        check(aliasDecl[1].trim() === type,
              `TradeData.${alias} is typed ${type} (declared "${aliasDecl[1].trim()}")`);
        const before = tradeBody.slice(0, tradeBody.indexOf(aliasDecl[0]));
        const comment = before.lastIndexOf('/**');
        check(comment >= 0 && /@deprecated/.test(before.slice(comment)),
              `TradeData.${alias} is marked @deprecated`);
      }
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
