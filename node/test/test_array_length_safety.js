// node/test/test_array_length_safety.js
//
// Node-side parity for the pybind11 array-length-check fix (see
// python/tests/test_array_length_safety.py): aggregateTimeBars read
// its element count from `timestamps` only, and barReturns/tradePnl
// read theirs from `signal_long` only, in both cases never checking
// that the other arrays actually had as many elements -- a caller
// passing a mismatched array read (and, once large enough, crashed)
// past its end. requireSameLength (node/src/bindings_common.h) now
// closes that gap the same way the batch indicators already did.

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

function throws(fn) {
  try {
    fn();
    return false;
  } catch (e) {
    return true;
  }
}

check('aggregateTimeBars rejects a mismatched prices array', () => {
  const ts = new Float64Array([0, 1e9, 2e9]);
  const px = new Float64Array([100.0]); // too short
  const qty = new Float64Array([1.0, 1.0, 1.0]);
  const ib = new Uint8Array([1, 1, 1]);
  assert.ok(throws(() => flox.aggregateTimeBars(ts, px, qty, ib, 1.0)));
});

check('aggregateTimeBars rejects a mismatched quantities array', () => {
  const ts = new Float64Array([0, 1e9]);
  const px = new Float64Array([100.0, 101.0]);
  const qty = new Float64Array([1.0]); // too short
  const ib = new Uint8Array([1, 1]);
  assert.ok(throws(() => flox.aggregateTimeBars(ts, px, qty, ib, 1.0)));
});

check('aggregateTimeBars rejects a mismatched isBuy array', () => {
  const ts = new Float64Array([0, 1e9]);
  const px = new Float64Array([100.0, 101.0]);
  const qty = new Float64Array([1.0, 1.0]);
  const ib = new Uint8Array([1]); // too short
  assert.ok(throws(() => flox.aggregateTimeBars(ts, px, qty, ib, 1.0)));
});

check('aggregateTimeBars still works on matching-length arrays', () => {
  const n = 20;
  const ts = new Float64Array(n);
  const px = new Float64Array(n);
  const qty = new Float64Array(n);
  const ib = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    ts[i] = i * 1e9;
    px[i] = 100.0 + i;
    qty[i] = 1.0;
    ib[i] = 1;
  }
  const bars = flox.aggregateTimeBars(ts, px, qty, ib, 1.0);
  // Not asserting an exact count here: the C-ABI aggregator's
  // last-bar-flush behavior is a separate, pre-existing concern from
  // the length check this test targets. What matters for THIS fix is
  // that matching-length arrays still aggregate without error.
  assert.ok(bars.length > 0);
});

check('barReturns rejects a mismatched signalShort array', () => {
  const sl = new Int8Array(10).fill(1);
  const ss = new Int8Array(2); // too short
  const lr = new Float64Array(10);
  assert.ok(throws(() => flox.barReturns(sl, ss, lr)));
});

check('barReturns rejects a mismatched logReturns array', () => {
  const sl = new Int8Array(10).fill(1);
  const ss = new Int8Array(10);
  const lr = new Float64Array(2); // too short
  assert.ok(throws(() => flox.barReturns(sl, ss, lr)));
});

check('tradePnl rejects a mismatched signalShort array', () => {
  const sl = new Int8Array(10).fill(1);
  const ss = new Int8Array(3); // too short
  const lr = new Float64Array(10);
  assert.ok(throws(() => flox.tradePnl(sl, ss, lr)));
});

check('barReturns still works on matching-length arrays', () => {
  const sl = new Int8Array([0, 1, 1, 0, 0]);
  const ss = new Int8Array([0, 0, 0, 0, 1]);
  const lr = new Float64Array([0.0, 0.01, 0.02, -0.01, 0.03]);
  const out = flox.barReturns(sl, ss, lr);
  assert.strictEqual(out.length, 5);
});

console.log(`${passed} check(s) passed`);
process.exit(process.exitCode || 0);
