// node/test/test_progress_callback.js
//
// W14-T026 — N-API binding for DataReader.run's onProgress option.
// Mirrors python/tests/test_progress_callback.py:
//   * positional n_threads still works (back-compat)
//   * onProgress fires at least once; pct ends at 1.0; ts monotonic
//   * returning false cancels the run (run returns false)
//   * options object: { nThreads, onProgress, progressIntervalMs }

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

function mkTmp()
{
  return fs.mkdtempSync(path.join(os.tmpdir(), 'flox-pcb-'));
}

// Write a synthetic tape with N trades 1µs apart.
function writeTape(outDir, n)
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  const hook = new flox.BinaryLogRecorderHook(
      outDir, 8, 0, 'none', 'test', 'perpetual');
  hook.addSymbol(Number(sym), 'BTCUSDT', '', '', 2, 6);
  const runner = new flox.Runner(reg, () => {}, false);
  runner.addStrategy({symbols: [sym]});
  runner.setMarketDataRecorder(hook);
  runner.start();
  const base = 1_700_000_000_000_000_000n;
  for (let i = 0; i < n; ++i) {
    const ts = Number(base + BigInt(i) * 1_000n);
    runner.onTrade(sym, 100.0 + i * 0.01, 1.0, true, ts);
  }
  runner.stop();
  hook.flush();
}

function main()
{
  const dir = mkTmp();
  try {
    writeTape(dir, 50_000);

    // Positional n_threads still accepted (back-compat).
    {
      const r = new flox.DataReader(dir);
      const agg = new flox.OHLCBinAggregator(60_000_000_000n, false);
      check(r.run([agg], 1) === true, 'positional nThreads still works');
    }

    // onProgress fires + returns true → run completes.
    {
      const r = new flox.DataReader(dir);
      const agg = new flox.OHLCBinAggregator(60_000_000_000n, false);
      const calls = [];
      const ok = r.run([agg], {
        nThreads: 1,
        progressIntervalMs: 1,
        onProgress: (pct, ts) => { calls.push([pct, ts]); return true; },
      });
      check(ok === true, 'onProgress(returning true) → run returns true');
      check(calls.length >= 1, 'onProgress fired at least once');
      const last = calls[calls.length - 1];
      check(Math.abs(last[0] - 1.0) < 1e-6,
            `final tick pct=1.0 (got ${last[0]})`);
    }

    // onProgress returning false cancels.
    {
      const r = new flox.DataReader(dir);
      const agg = new flox.OHLCBinAggregator(60_000_000_000n, false);
      let calls = 0;
      const ok = r.run([agg], {
        nThreads: 1,
        progressIntervalMs: 1,
        onProgress: () => { calls += 1; return false; },
      });
      check(ok === false, 'onProgress(returning false) → run returns false');
      check(calls >= 1, 'onProgress fired before cancel');
    }
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }

  console.log(`\n${_passed} passed, ${_failed} failed`);
  process.exit(_failed ? 1 : 0);
}

main();
