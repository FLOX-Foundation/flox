// node/test/test_backtest_run_tapes.js
//
// W14-T017 — N-API BacktestRunner.runTapes(paths).
// Mirrors the Python suite in
// python/tests/test_merged_tape_reader.py::test_run_tapes_*:
//   * runTapes([t]) ≡ runTape(t)   — same stats shape/keys
//   * runTapes([t1, t2]) runs to completion on two different-venue tapes
//   * runTapes([]) throws

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const flox = require('..');

let _failed = 0;
let _passed = 0;
function check(cond, msg)
{
  if (cond)
  {
    _passed++;
    console.log(`  ok  - ${msg}`);
  }
  else
  {
    _failed++;
    console.log(`  FAIL - ${msg}`);
  }
}

function mkTmpDir(label)
{
  return fs.mkdtempSync(path.join(os.tmpdir(), `flox-rt-${label}-`));
}

// Write a `.floxlog` tape via the runtime path: SymbolRegistry +
// Runner + BinaryLogRecorderHook. Mirrors writeTape() in
// test_merged_tape_reader.js (which mirrors Python `_write_tape`).
function writeTape(outDir, exchange, symbolName, trades, exchangeId)
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol(exchange, symbolName, 0.01);
  const hook = new flox.BinaryLogRecorderHook(
      outDir, 4, exchangeId, 'none', exchange, 'perpetual');
  hook.addSymbol(Number(sym), symbolName, '', '', 2, 6);
  const runner = new flox.Runner(reg, () => {}, false);
  runner.addStrategy({symbols: [sym]});
  runner.setMarketDataRecorder(hook);
  runner.start();
  for (const [tsNs, price, qty, isBuy] of trades)
  {
    // Runner.onTrade currently accepts only Number for the timestamp;
    // keep test timestamps under 2^53 to round-trip losslessly.
    runner.onTrade(Number(sym), price, qty, isBuy, Number(tsNs));
  }
  runner.stop();
  hook.flush();
}

// Build a fresh BacktestRunner + no-op strategy so the runTape/runTapes
// gate accepts the call. Strategy makes no trades — we're proving the
// stats shape/keys, not P&L.
function freshBt(exchange, symbolName)
{
  const reg = new flox.SymbolRegistry();
  reg.addSymbol(exchange, symbolName, 0.01);
  const bt = new flox.BacktestRunner(reg, 0.0004, 10000);
  bt.setStrategy({symbols: [], onTrade(_ctx, _t, _emit) {}});
  return bt;
}

// ── 1. runTapes([t]) ≡ runTape(t) ────────────────────────────────────

function testSingleTapeEquivalence()
{
  console.log('test_run_tapes_single_equals_run_tape');
  const d = mkTmpDir('single');
  const tape = path.join(d, 'bybit');
  // Keep below 2^53 so runner.onTrade's Number path doesn't truncate.
  const base = 1_000_000_000_000n;
  const trades = [];
  for (let i = 0; i < 5; i++)
  {
    trades.push([base + BigInt(i) * 1_000_000n, 50000.0 + i, 0.1, i % 2 === 0]);
  }
  writeTape(tape, 'bybit', 'BTCUSDT', trades, 1);

  const single = freshBt('bybit', 'BTCUSDT').runTape(tape);
  const multi = freshBt('bybit', 'BTCUSDT').runTapes([tape]);

  check(single !== null && typeof single === 'object',
        'runTape returned a stats object');
  check(multi !== null && typeof multi === 'object',
        'runTapes returned a stats object');

  // Keys must match — same statsToJs shape.
  const singleKeys = Object.keys(single).sort();
  const multiKeys = Object.keys(multi).sort();
  check(singleKeys.join(',') === multiKeys.join(','),
        `keys match (got ${multiKeys.join(',')})`);

  // Core numeric fields are identical (deterministic engine pipeline,
  // same event stream).
  const fields = ['totalTrades', 'winningTrades', 'losingTrades',
                  'initialCapital', 'finalCapital', 'netPnl',
                  'totalFees', 'returnPct'];
  for (const k of fields)
  {
    check(single[k] === multi[k],
          `${k}: runTape == runTapes (${single[k]} == ${multi[k]})`);
  }

  fs.rmSync(d, {recursive: true, force: true});
}

// ── 2. runTapes([t1, t2]) on two different-venue tapes ───────────────

function testTwoTapesDifferentVenues()
{
  console.log('test_run_tapes_two_venues');
  const d = mkTmpDir('twovenue');
  const t1 = path.join(d, 'bybit');
  const t2 = path.join(d, 'binance');
  // Keep below 2^53 so runner.onTrade's Number path doesn't truncate.
  const base = 1_000_000_000_000n;
  const tA = [];
  for (let i = 0; i < 4; i++)
  {
    tA.push([base + BigInt(i) * 1_000_000n, 50000.0 + i, 0.1, true]);
  }
  const tB = [];
  for (let i = 0; i < 3; i++)
  {
    tB.push(
        [base + 500_000n + BigInt(i) * 1_000_000n, 3000.0 + i, 1.0, false]);
  }
  writeTape(t1, 'bybit', 'BTCUSDT', tA, 1);
  writeTape(t2, 'binance', 'ETHUSDT', tB, 2);

  // Strategy needs both venues' symbols registered in the bt registry
  // so it can resolve them; concrete symbol IDs in the merged stream
  // are rekeyed by (exchange, name) so the strategy never sees them
  // directly here (no orders emitted).
  const reg = new flox.SymbolRegistry();
  reg.addSymbol('bybit', 'BTCUSDT', 0.01);
  reg.addSymbol('binance', 'ETHUSDT', 0.01);
  const bt = new flox.BacktestRunner(reg, 0.0004, 10000);
  bt.setStrategy({symbols: [], onTrade(_ctx, _t, _emit) {}});

  let stats = null;
  let threw = null;
  try { stats = bt.runTapes([t1, t2]); }
  catch (e) { threw = e; }
  check(threw === null,
        `runTapes([t1, t2]) did not throw (${threw && threw.message})`);
  check(stats !== null && typeof stats === 'object',
        'runTapes([t1, t2]) returned a stats object');
  check('totalTrades' in stats && 'finalCapital' in stats,
        'stats has totalTrades + finalCapital');
  check(typeof stats.finalCapital === 'number'
            && Number.isFinite(stats.finalCapital),
        `finalCapital is finite number (got ${stats.finalCapital})`);

  fs.rmSync(d, {recursive: true, force: true});
}

// ── 3. runTapes([]) throws ───────────────────────────────────────────

function testEmptyPathsThrows()
{
  console.log('test_run_tapes_empty_paths_throws');
  const bt = freshBt('bybit', 'BTCUSDT');
  let threw = false;
  let msg = '';
  try { bt.runTapes([]); }
  catch (e)
  {
    threw = true;
    msg = String(e && e.message ? e.message : e);
  }
  check(threw, `runTapes([]) throws (got msg: "${msg}")`);
}

// ── driver ───────────────────────────────────────────────────────────

testSingleTapeEquivalence();
testTwoTapesDifferentVenues();
testEmptyPathsThrows();

console.log(`\n${_passed} passed, ${_failed} failed`);
if (_failed > 0)
{
  process.exit(1);
}
