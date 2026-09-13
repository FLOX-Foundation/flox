// node/test/test_reader_ordering.js
//
// Ordering contract of DataReader on a tape that is not perfectly sorted.
// Mirrors python/tests/test_replay_reader_ordering.py:
//   * an event past the reorder window is dropped and counted, not thrown
//   * strictOrdering restores the throw, with symbol and offset in the text
//   * a wide enough window keeps every event
//   * stats() carries lateDropped and unknownFramesSkipped

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
  return fs.mkdtempSync(path.join(os.tmpdir(), 'flox-order-'));
}

const SEC = 1_000_000_000n;
const BASE = 1_700_000_000n * SEC;

// 50 trades on symbol 1, one on symbol 2 stamped 40s back, then 50 more.
function writeLaggingTape(outDir)
{
  const w = new flox.DataWriter(outDir, 4, 0);
  for (let i = 0; i < 50; ++i) {
    const ts = BASE + BigInt(i) * SEC;
    w.writeTrade(ts, ts, 100.0, 1.0, i, 1, 0);
  }
  const late = BASE + 9n * SEC;
  w.writeTrade(late, late, 100.0, 1.0, 999, 2, 0);
  for (let i = 50; i < 100; ++i) {
    const ts = BASE + BigInt(i) * SEC;
    w.writeTrade(ts, ts, 100.0, 1.0, i, 1, 0);
  }
  w.close();
}

function main()
{
  const dir = mkTmp();
  try {
    writeLaggingTape(dir);

    {
      const r = new flox.DataReader(dir, undefined, undefined,
                                    {reorderWindowNs: 10n * SEC});
      const agg = new flox.OHLCBinAggregator(60_000_000_000n, false);
      let threw = false;
      try {
        r.run([agg], 1);
      } catch (e) {
        threw = true;
      }
      check(!threw, 'a late event does not end the walk');
      const s = r.stats();
      check(s.lateDropped === 1, `lateDropped === 1 (got ${s.lateDropped})`);
      check(s.eventsRead === 100, `eventsRead === 100 (got ${s.eventsRead})`);
      check(s.unknownFramesSkipped === 0, 'unknownFramesSkipped === 0');
    }

    {
      // The C ABI has no error channel yet, so a C++ throw becomes a false
      // return here rather than a JS exception. The engine and the Python
      // binding carry the full message; see index.d.ts on strictOrdering.
      const r = new flox.DataReader(dir, undefined, undefined,
                                    {reorderWindowNs: 10n * SEC, strictOrdering: true});
      const agg = new flox.OHLCBinAggregator(60_000_000_000n, false);
      const ok = r.run([agg], 1);
      check(ok === false, 'strictOrdering stops the run on the late event');
      check(r.stats().lateDropped === 0,
            'strictOrdering counts nothing as dropped (it stops instead)');
    }

    {
      const r = new flox.DataReader(dir, undefined, undefined,
                                    {reorderWindowNs: 60n * SEC});
      const agg = new flox.OHLCBinAggregator(60_000_000_000n, false);
      r.run([agg], 1);
      const s = r.stats();
      check(s.lateDropped === 0, 'a wide window drops nothing');
      check(s.eventsRead === 101, `eventsRead === 101 (got ${s.eventsRead})`);
    }
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }

  console.log(`\n${_passed} passed, ${_failed} failed`);
  process.exit(_failed === 0 ? 0 : 1);
}

main();
