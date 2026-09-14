'use strict';
/**
 * node/test/test_order_type_codes.js — regression test:
 * `SimulatedExecutor.submitOrder` used to encode the order-type
 * string in the FLOX_SIGNAL_TYPE_* space (market=0, limit=1) and hand the
 * result to a C function that reads the flox::OrderType space (LIMIT=0,
 * MARKET=1). A "market" order was submitted as a LIMIT order and a "limit"
 * order was submitted as a MARKET order.
 *
 * Reference behavior: a market buy fills on the first print regardless of
 * price; a limit buy priced below the market must NOT fill until the
 * market actually reaches that price. Before the fix this was exactly
 * backwards: "market" sat unfilled and "limit" filled instantly at any
 * price.
 *
 * Run from repo root:
 *   cd node && node test/test_order_type_codes.js
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function throws(fn, needle, msg) {
  let threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw && String(threw.message).includes(needle)) {
    passed++;
    console.log(`  ok  ${msg}`);
  } else {
    failed++;
    console.error(`  FAIL  ${msg} (threw: ${threw && threw.message})`);
  }
}

console.log('=== SimulatedExecutor.submitOrder order-type encoding ===');
{
  const exec = new flox.SimulatedExecutor();
  const SYM = 1;

  // A market buy fills on the very first print, regardless of the price
  // argument (the engine ignores price for MARKET orders).
  exec.submitOrder(1, 'buy', /*price=*/1, /*qty=*/1.0, 'market', SYM);
  check(exec.fillCount === 0, 'market order does not fill before any print');
  exec.onBar(SYM, 100.0);
  check(exec.fillCount === 1, 'market buy fills on the first print (100.0)');

  // A limit buy priced well below the current market must not fill while
  // the market stays above it...
  exec.submitOrder(2, 'buy', /*price=*/50, /*qty=*/1.0, 'limit', SYM);
  exec.onBar(SYM, 100.0);
  check(exec.fillCount === 1, 'limit buy at 50 does not fill while market prints at 100');

  // ...and fills once the market actually reaches the limit price.
  exec.onBar(SYM, 50.0);
  check(exec.fillCount === 2, 'limit buy at 50 fills once the market reaches 50');
}

console.log('=== submitOrder rejects an unrecognized order type ===');
{
  const exec = new flox.SimulatedExecutor();
  throws(() => exec.submitOrder(1, 'buy', 100, 1.0, 'not_a_real_type', 1),
         'unknown order type', 'submitOrder throws instead of silently defaulting');
}

// orderToJs (Executor.submit → lower-case) and orderTypeName
// (Strategy.onFill/onOrderUpdate → upper-case) decode the same
// flox::OrderType space from two independently maintained call sites.
// They must never disagree on a code both of them define. Each needs its
// own BacktestRunner: a custom Executor bypasses the built-in simulated
// fill loop, so onFill only fires when the default executor is left in
// place.
function runOneBarMarketBuy() {
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const btr = new flox.BacktestRunner(reg, 0.0, 10000.0);
  let fired = false;
  btr.setStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) {
      if (fired) return;
      fired = true;
      emit.marketBuy(Number(sym), 1.0);
    },
  });
  return { btr, sym };
}
function runBars(btr, sym) {
  btr.runBars(
    new BigInt64Array([1_000_000_000n]),
    new BigInt64Array([1_999_999_999n]),
    new Float64Array([100.0]),
    new Float64Array([101.0]),
    new Float64Array([99.0]),
    new Float64Array([100.5]),
    new Float64Array([10.0]),
    'BTC');
}

console.log('=== orderToJs (Executor.submit) decodes MARKET as "market" ===');
{
  const { btr, sym } = runOneBarMarketBuy();
  const submits = [];
  btr.setExecutor({ submit(o) { submits.push(o); } });
  runBars(btr, sym);
  check(submits.length === 1, `Executor.submit fired once (got ${submits.length})`);
  check(submits[0]?.orderType === 'market', `Executor.submit orderType='market' (${submits[0]?.orderType})`);
}

console.log('=== orderTypeName (Strategy.onFill) decodes MARKET as "MARKET" ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const btr = new flox.BacktestRunner(reg, 0.0, 10000.0);
  const fills = [];
  let fired = false;
  btr.setStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) {
      if (fired) return;
      fired = true;
      emit.marketBuy(Number(sym), 1.0);
    },
    onFill(_ctx, ev) { fills.push(ev); },
  });
  runBars(btr, sym);
  check(fills.length >= 1, `Strategy.onFill fired (got ${fills.length})`);
  check(fills[0]?.orderType === 'MARKET', `onFill orderType='MARKET' (${fills[0]?.orderType})`);
}

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
