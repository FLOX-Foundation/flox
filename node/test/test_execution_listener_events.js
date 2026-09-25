'use strict';
/**
 * node/test/test_execution_listener_events.js -- the order-lifecycle
 * events a BacktestRunner's execution listener receives, and the shape
 * of what they carry.
 *
 * node/test/test_hooks.js attaches a listener with a single onFilled and
 * counts it. Nothing checked the payload, nothing checked the order the
 * events arrive in, and nothing checked that a listener object defining
 * only some of the twelve handlers is safe -- each bridge in
 * node/src/hooks.h guards on its own FunctionReference being empty, and
 * a missing guard is a call on an empty reference.
 *
 * The four events the fix restored -- onPartiallyFilled, onRejected,
 * onReplaced, onTrailingStopUpdated -- are covered here by the generic
 * payload check rather than by a scenario of their own, because no
 * scenario reaches them from JavaScript today:
 *
 *   - a partial fill needs SimulatedExecutor's queue tracker, and the
 *     runner builds its executor from a default BacktestConfig whose
 *     queueModel is NONE; Node's BacktestRunner takes (registry, feeRate,
 *     initialCapital) and exposes no queue-model setter, and there is no
 *     C entry point attaching an already-configured SimulatedExecutor to
 *     a runner.
 *   - every reject path in the simulator keys off an order flag
 *     (reduceOnly, postOnly, FOK), a rate-limit policy, an STP scope, or
 *     a non-zero cancel/replace ack latency. A strategy's emit surface is
 *     marketBuy / marketSell / limitBuy / limitSell / cancel /
 *     closePosition / provideLiquidity / withdrawLiquidity -- it sets
 *     none of those.
 *   - a replace needs replaceOrder and a trailing update needs a
 *     trailing-stop order; neither is on the emit surface.
 *
 * So the listener below defines all twelve handlers and validates every
 * payload that does arrive. The day any of the four becomes reachable,
 * it is checked the same way as the rest without a change here.
 *
 * Run from repo root:
 *   cd node && node test/test_execution_listener_events.js
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

function describe(v) {
  return JSON.stringify(v, (_k, x) => (typeof x === 'bigint' ? `${x}n` : x));
}

const ALL_HANDLERS = [
  'onSubmitted', 'onAccepted', 'onPartiallyFilled', 'onFilled',
  'onPendingCancel', 'onCanceled', 'onExpired', 'onRejected',
  'onReplaced', 'onPendingTrigger', 'onTriggered', 'onTrailingStopUpdated',
];

// Handlers whose first argument is an order and which take a second
// argument, with the type that second argument must have.
const SECOND_ARG = {
  onPartiallyFilled: 'number',
  onRejected: 'string',
  onReplaced: 'object',
  onTrailingStopUpdated: 'number',
};

// Every field node/src/hooks.h's orderToJs sets, with the JS type it
// must carry.
const ORDER_FIELDS = {
  id: 'number',
  clientOrderId: 'number',
  symbol: 'number',
  strategyId: 'number',
  orderTag: 'number',
  side: 'string',
  orderType: 'string',
  timeInForce: 'string',
  reduceOnly: 'boolean',
  closePosition: 'boolean',
  postOnly: 'boolean',
  price: 'number',
  quantity: 'number',
  filledQuantity: 'number',
  triggerPrice: 'number',
  trailingOffset: 'number',
  createdAtNs: 'bigint',
  exchangeTsNs: 'bigint',
};

function badOrderPayload(order) {
  if (order === null || typeof order !== 'object') {
    return `not an object: ${describe(order)}`;
  }
  for (const [field, type] of Object.entries(ORDER_FIELDS)) {
    if (typeof order[field] !== type) {
      return `${field} is ${typeof order[field]}, want ${type}`;
    }
  }
  if (order.side !== 'buy' && order.side !== 'sell') {
    return `side is ${describe(order.side)}`;
  }
  return null;
}

// A listener that answers to all twelve names, records the sequence, and
// validates every payload it is handed.
function recordingListener() {
  const events = [];
  const payloadProblems = [];
  const listener = {};
  for (const name of ALL_HANDLERS) {
    listener[name] = (order, second) => {
      events.push({ name, order, second });
      const problem = badOrderPayload(order);
      if (problem !== null) {
        payloadProblems.push(`${name}: ${problem}`);
      }
      const wantSecond = SECOND_ARG[name];
      if (wantSecond !== undefined && typeof second !== wantSecond) {
        payloadProblems.push(`${name}: second argument is ${typeof second}, want ${wantSecond}`);
      }
    };
  }
  return { listener, events, payloadProblems, names: () => events.map(e => e.name) };
}

const START = [1000000000n, 2000000000n, 3000000000n, 4000000000n];
const END = [1999999999n, 2999999999n, 3999999999n, 4999999999n];

function replay(runner) {
  runner.runBars(
    new BigInt64Array(START), new BigInt64Array(END),
    new Float64Array([100.0, 100.5, 101.0, 101.5]),
    new Float64Array([101.0, 101.5, 102.0, 102.5]),
    new Float64Array([99.0, 99.5, 100.0, 100.5]),
    new Float64Array([100.5, 101.0, 101.5, 102.0]),
    new Float64Array([10.0, 10.0, 10.0, 10.0]),
    'BTC');
}

function scenario(onBar) {
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.BacktestRunner(reg, 0.0, 10000.0);
  const rec = recordingListener();
  runner.addExecutionListener(rec.listener);
  let bar = 0;
  runner.setStrategy({
    symbols: [sym],
    onBar(_ctx, _b, emit) { bar++; onBar(emit, Number(sym), bar, rec); },
  });
  replay(runner);
  return rec;
}

console.log('=== A market order walks submitted -> accepted -> filled ===');
{
  const rec = scenario((emit, sym, bar) => { if (bar === 1) { emit.marketBuy(2.5, sym); } });
  const names = rec.names();
  check(names.length >= 3, `at least three events arrived (got ${names.length}: ${names.join(', ')})`);
  check(names[0] === 'onSubmitted', `first event is onSubmitted (got ${names[0]})`);
  check(names[1] === 'onAccepted', `second event is onAccepted (got ${names[1]})`);
  check(names.includes('onFilled'), `the order filled (sequence was ${names.join(', ')})`);
  check(rec.payloadProblems.length === 0,
        `every payload is well formed (${rec.payloadProblems.join('; ') || 'none'})`);

  const filled = rec.events.find(e => e.name === 'onFilled');
  if (filled) {
    check(filled.order.side === 'buy', `onFilled order side is buy (got ${filled.order.side})`);
    check(filled.order.orderType === 'market',
          `onFilled order type is market (got ${filled.order.orderType})`);
    check(Math.abs(filled.order.quantity - 2.5) < 1e-9,
          `onFilled order quantity is the emitted 2.5 (got ${filled.order.quantity})`);
    check(Math.abs(filled.order.filledQuantity - 2.5) < 1e-9,
          `onFilled reports the whole quantity filled (got ${filled.order.filledQuantity})`);
    check(filled.order.symbol === 1, `onFilled order carries the symbol id (got ${filled.order.symbol})`);
    const submitted = rec.events.find(e => e.name === 'onSubmitted');
    check(submitted && submitted.order.id === filled.order.id,
          `the same order id runs through the sequence (${submitted && submitted.order.id} vs ${filled.order.id})`);
  }
}

console.log('\n=== A resting limit order that is cancelled reports onCanceled ===');
{
  const rec = scenario((emit, sym, bar, self) => {
    if (bar === 1) { emit.limitBuy(1.0, 1.0, sym); }
    if (bar === 3) {
      const submitted = self.events.find(e => e.name === 'onSubmitted');
      if (submitted) { emit.cancel(submitted.order.id); }
    }
  });
  const names = rec.names();
  check(names.includes('onCanceled'), `onCanceled arrived (sequence was ${names.join(', ')})`);
  check(!names.includes('onFilled'), `a bid far below the market never filled (${names.join(', ')})`);
  check(rec.payloadProblems.length === 0,
        `every payload is well formed (${rec.payloadProblems.join('; ') || 'none'})`);
  const canceled = rec.events.find(e => e.name === 'onCanceled');
  if (canceled) {
    check(canceled.order.orderType === 'limit',
          `onCanceled order type is limit (got ${canceled.order.orderType})`);
    check(Math.abs(canceled.order.price - 1.0) < 1e-9,
          `onCanceled order keeps its limit price (got ${canceled.order.price})`);
  }
}

console.log('\n=== A listener defining only some handlers is safe ===');
{
  // Every bridge guards on its own reference being empty; a missing
  // guard is a call through an empty FunctionReference.
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.BacktestRunner(reg, 0.0, 10000.0);
  const fills = [];
  runner.addExecutionListener({ onFilled(o) { fills.push(o); } });
  let bar = 0;
  runner.setStrategy({
    symbols: [sym],
    onBar(_ctx, _b, emit) { bar++; if (bar === 1) { emit.marketBuy(1.0, Number(sym)); } },
  });
  let err = null;
  try { replay(runner); } catch (e) { err = e; }
  check(err === null, `a one-handler listener runs clean${err ? ` (threw ${err.message})` : ''}`);
  check(fills.length >= 1, `its onFilled still fired (got ${fills.length})`);
}

console.log('\n=== An empty listener object is accepted and never called ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.BacktestRunner(reg, 0.0, 10000.0);
  let err = null;
  try {
    runner.addExecutionListener({});
    let bar = 0;
    runner.setStrategy({
      symbols: [sym],
      onBar(_ctx, _b, emit) { bar++; if (bar === 1) { emit.marketBuy(1.0, Number(sym)); } },
    });
    replay(runner);
  } catch (e) { err = e; }
  check(err === null, `an empty listener runs clean${err ? ` (threw ${err.message})` : ''}`);
}

console.log('\n=== Every attached listener sees every event ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.BacktestRunner(reg, 0.0, 10000.0);
  const a = recordingListener();
  const b = recordingListener();
  runner.addExecutionListener(a.listener);
  runner.addExecutionListener(b.listener);
  let bar = 0;
  runner.setStrategy({
    symbols: [sym],
    onBar(_ctx, _b, emit) { bar++; if (bar === 1) { emit.marketBuy(1.0, Number(sym)); } },
  });
  replay(runner);
  check(a.names().length > 0, `the first listener saw events (got ${a.names().length})`);
  check(a.names().join(',') === b.names().join(','),
        `both listeners saw the same sequence (${a.names().join(',')} vs ${b.names().join(',')})`);
}

console.log('\n=== index.d.ts declares all twelve handlers ===');
{
  const dts = fs.readFileSync(path.join(__dirname, '..', 'index.d.ts'), 'utf8');
  const iface = dts.match(/export interface ExecutionListener\s*\{([\s\S]*?)\n\}/);
  check(iface !== null, 'index.d.ts declares interface ExecutionListener');
  if (iface !== null) {
    for (const name of ALL_HANDLERS) {
      check(new RegExp(`\\b${name}\\?\\(`).test(iface[1]),
            `ExecutionListener declares ${name}`);
    }
  }
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
