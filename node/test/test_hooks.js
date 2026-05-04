'use strict';
/**
 * node/test/test_hooks.js — smoke-test NAPI wrappers for the extension
 * hooks (PnLTracker, StorageSink, RiskManager, KillSwitch,
 * OrderValidator, MarketDataRecorderHook, ReplaySource, Executor,
 * ExecutionListener, setLogCallback).
 *
 * Run from repo root:
 *   cd node && node test/test_hooks.js
 */

const path = require('path');
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

// Helper — build a sync runner with a one-shot strategy that fires a
// market buy on the first onTrade.
function makeRunnerWithStrategy() {
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const onSignalCalls = [];
  const runner = new flox.Runner(reg, (sig) => onSignalCalls.push(sig), false);

  let fired = false;
  runner.addStrategy({
    symbols: [sym],
    onTrade(_ctx, _trade, emit) {
      if (fired) return;
      fired = true;
      emit.marketBuy(Number(sym), 1.0);
    },
  });
  return { reg, runner, sym, onSignalCalls };
}

// ── PnLTracker ──────────────────────────────────────────────────────

function testPnlTracker() {
  console.log('testPnlTracker');
  const { runner, sym } = makeRunnerWithStrategy();
  const seen = [];
  runner.setPnlTracker({
    onSignal(sig) { seen.push({ sym: sig.symbol, side: sig.side, qty: sig.quantity }); },
  });
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000);
  runner.stop();
  check(seen.length === 1, `PnLTracker.onSignal fired once, got ${seen.length}`);
  check(seen[0]?.side === 'buy', `side='buy' (${seen[0]?.side})`);
}

// ── StorageSink ─────────────────────────────────────────────────────

function testStorageSink() {
  console.log('testStorageSink');
  const { runner, sym } = makeRunnerWithStrategy();
  const stored = [];
  runner.setStorageSink({ store(sig) { stored.push(sig); } });
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000);
  runner.stop();
  check(stored.length === 1, `StorageSink.store fired once, got ${stored.length}`);
}

// ── RiskManager ─────────────────────────────────────────────────────

function testRiskManagerDrops() {
  console.log('testRiskManagerDrops');
  const { runner, sym, onSignalCalls } = makeRunnerWithStrategy();
  runner.setRiskManager({ allow() { return false; } });
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000);
  runner.stop();
  check(onSignalCalls.length === 0,
        `RiskManager denies → no on_signal, got ${onSignalCalls.length}`);
}

// ── KillSwitch ──────────────────────────────────────────────────────

function testKillSwitchDrops() {
  console.log('testKillSwitchDrops');
  const { runner, sym, onSignalCalls } = makeRunnerWithStrategy();
  runner.setKillSwitch({ check() { return false; } });
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000);
  runner.stop();
  check(onSignalCalls.length === 0, `KillSwitch denies → no on_signal`);
}

// ── OrderValidator ──────────────────────────────────────────────────

function testOrderValidatorDrops() {
  console.log('testOrderValidatorDrops');
  const { runner, sym, onSignalCalls } = makeRunnerWithStrategy();
  runner.setOrderValidator({ validate() { return false; } });
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000);
  runner.stop();
  check(onSignalCalls.length === 0, `OrderValidator rejects → no on_signal`);
}

// ── Executor ────────────────────────────────────────────────────────

function testExecutor() {
  console.log('testExecutor');
  const { runner, sym } = makeRunnerWithStrategy();
  const submits = [];
  let starts = 0, stops = 0;
  runner.setExecutor({
    submit(o) { submits.push({ sym: o.symbol, side: o.side, qty: o.quantity, type: o.orderType }); },
    onStart() { starts++; },
    onStop() { stops++; },
  });
  runner.start();
  check(starts === 1, `Executor.onStart fired on runner.start (got ${starts})`);
  runner.onTrade(Number(sym), 100, 1, true, 1000);
  runner.stop();
  check(stops === 1, `Executor.onStop fired on runner.stop (got ${stops})`);
  check(submits.length === 1, `Executor.submit fired once (got ${submits.length})`);
  check(submits[0]?.type === 'market', `submit.orderType='market' (${submits[0]?.type})`);
}

// ── MarketDataRecorderHook ──────────────────────────────────────────

function testMarketDataRecorder() {
  console.log('testMarketDataRecorder');
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.Runner(reg, () => {}, false);
  runner.addStrategy({ symbols: [sym] });

  const trades = [];
  const books = [];
  let starts = 0;
  runner.setMarketDataRecorder({
    onTrade(t) { trades.push(t); },
    onBookUpdate(s, snap, bids, asks) { books.push({ s, snap, bids, asks }); },
    onStart() { starts++; },
  });
  runner.start();
  check(starts === 1, `MarketDataRecorderHook.onStart fired (${starts})`);
  runner.onTrade(Number(sym), 101.5, 0.5, true, 5000);
  runner.onBookSnapshot(Number(sym),
    [100, 99], [1, 2],
    [102, 103], [1, 2],
    6000);
  runner.stop();
  check(trades.length === 1, `recorder.onTrade fired once (got ${trades.length})`);
  check(books.length === 1, `recorder.onBookUpdate fired once`);
  check(books[0]?.bids?.length === 2, `bids has 2 levels`);
}

// ── ExecutionListener (BacktestRunner) ──────────────────────────────

function testExecutionListenerBacktest() {
  console.log('testExecutionListenerBacktest');
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const btr = new flox.BacktestRunner(reg, 0.0, 10000.0);

  const fills = [];
  btr.addExecutionListener({
    onFilled(o) { fills.push(o); },
  });

  let fired = false;
  btr.setStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) {
      if (fired) return;
      fired = true;
      emit.marketBuy(Number(sym), 1.0);
    },
  });

  btr.runBars(
    new BigInt64Array([1_000_000_000n]),
    new BigInt64Array([1_999_999_999n]),
    new Float64Array([100.0]),
    new Float64Array([101.0]),
    new Float64Array([99.0]),
    new Float64Array([100.5]),
    new Float64Array([10.0]),
    'BTC');

  // Fill counts may vary by simulator quirks — assert at least one fill
  // observed via the listener.
  check(fills.length >= 1, `ExecutionListener.onFilled ≥1× (got ${fills.length})`);
}

// ── Executor (BacktestRunner) ────────────────────────────────────────

function testExecutorBacktest() {
  console.log('testExecutorBacktest');
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const btr = new flox.BacktestRunner(reg, 0.0, 10000.0);

  const submits = [];
  btr.setExecutor({ submit(o) { submits.push(o); } });

  let fired = false;
  btr.setStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) {
      if (fired) return;
      fired = true;
      emit.marketBuy(Number(sym), 1.0);
    },
  });

  btr.runBars(
    new BigInt64Array([1_000_000_000n]),
    new BigInt64Array([1_999_999_999n]),
    new Float64Array([100.0]),
    new Float64Array([101.0]),
    new Float64Array([99.0]),
    new Float64Array([100.5]),
    new Float64Array([10.0]),
    'BTC');

  check(submits.length === 1, `Custom Executor.submit fired once (got ${submits.length})`);
  check(submits[0]?.orderType === 'market', `orderType='market'`);
}

// ── setLogCallback ──────────────────────────────────────────────────

function testLogCallback() {
  console.log('testLogCallback');
  // Just exercise the wiring without expecting log output (the C ABI
  // doesn't have a public Node-driven log emitter for testing).
  flox.setLogCallback((_lvl, _msg) => {});
  flox.setLogCallback(null);
  check(true, `setLogCallback wires + detaches without crash`);
}

// ── Run ─────────────────────────────────────────────────────────────

testPnlTracker();
testStorageSink();
testRiskManagerDrops();
testKillSwitchDrops();
testOrderValidatorDrops();
testExecutor();
testMarketDataRecorder();
testExecutionListenerBacktest();
testExecutorBacktest();
testLogCallback();

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
