// node/test/test_renko_gap_bricks.js
//
// Node-side parity for the Renko gap-synthesis fix (see
// python/tests/test_renko_gap_bricks.py and the RenkoBarPolicyTest cases in
// tests/test_bar_aggregator.cpp): a trade that gaps past more than one brick
// width used to collapse into a single zero-range bar, silently dropping
// every brick in between. It also doubles as the regression test for
// agg_renko_bars() sizing its output buffer to the trade count -- a bug that
// gap synthesis exposed (Renko can now emit more bars than there were
// trades) and that a debug build under AddressSanitizer turns into a
// heap-buffer-overflow read; see node/src/aggregators.h.
//
// Node's aggregate*Bars functions go through the shared C ABI aggregator
// (flox_aggregate_renko_bars). Python's pybind11 batch path
// (python/aggregator_bindings.h) used to disagree with it: Python flushed
// the still-open trailing bar and Node never did, so Python's count ran
// one higher than Node's on identical input. That gap is closed -- neither
// path returns the trailing bar now. Bar carries no closed/open flag, so
// returning the still-forming bar would hand the caller something
// indistinguishable from a real closed one, even though its high/low/
// close can still change on the next trade; dropping it is the only
// choice that doesn't forge a close. See python/tests/test_renko_gap_bricks.py
// for the same counts asserted on the Python side.

const assert = require('assert');
const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;

function check(label, fn) {
  try {
    fn();
    console.log(`  ok  ${label}`);
    passed++;
  } catch (e) {
    console.error(`  FAIL  ${label}: ${e.message}`);
    process.exitCode = 1;
  }
}

check('aggregateRenkoBars synthesizes the bricks a 5.5-brick gap spans', () => {
  const ts = new Float64Array([0, 1e9]);
  const px = new Float64Array([100.0, 155.0]); // gaps 5.5 bricks past 100
  const qty = new Float64Array([1.0, 1.0]);
  const ib = new Uint8Array([1, 1]);

  const bars = flox.aggregateRenkoBars(ts, px, qty, ib, 10.0);

  // The real bar (100 -> 110, closed at the first boundary the gapping
  // trade crossed) plus 4 synthesized bricks -- 5 bars. No bar for the new
  // brick the C ABI leaves open at 150; see the file comment.
  assert.strictEqual(bars.length, 5, '1 real + 4 synthesized bricks');

  assert.strictEqual(bars[0].open, 100);
  assert.strictEqual(bars[0].close, 110);

  const expectedOpens = [110, 120, 130, 140];
  const expectedCloses = [120, 130, 140, 150];
  for (let i = 0; i < 4; i++) {
    assert.strictEqual(bars[i + 1].open, expectedOpens[i], `brick ${i} open`);
    assert.strictEqual(bars[i + 1].close, expectedCloses[i], `brick ${i} close`);
  }
});

check('aggregateRenkoBars leaves an ordinary single-brick close alone', () => {
  const ts = new Float64Array([0, 1e9]);
  const px = new Float64Array([100.0, 114.0]); // 1.4 bricks -- one whole brick
  const qty = new Float64Array([1.0, 1.0]);
  const ib = new Uint8Array([1, 1]);

  const bars = flox.aggregateRenkoBars(ts, px, qty, ib, 10.0);

  assert.strictEqual(bars.length, 1, 'one closed brick, no trailing bar for the open one at 110 -> 114');
  assert.strictEqual(bars[0].open, 100);
  assert.strictEqual(bars[0].close, 110, 'a brick closes at its boundary, not at the crossing trade');
});

if (passed === 2 && process.exitCode === undefined) {
  console.log(`${passed}/2 passed`);
}
