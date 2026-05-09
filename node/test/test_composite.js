// Composite-condition DSL parity test for the Node binding (W1-T028).
// The DSL is pure JavaScript sugar built on top of `lastNClosedBars`,
// so we drive it with a fake strategy holding a synthetic bar ring.
// End-to-end engine integration is exercised by the QuickJS suite in
// tests/test_quickjs.cpp and by the pybind11 tests under
// python/tests/test_composite_conditions.py — same DSL shape, same
// inputs.

const { when, BAR_TYPE_TIME } = require('../lib/composite');

function check(label, ok) {
  if (!ok) { console.error('FAIL: ' + label); process.exit(1); }
  console.log('ok: ' + label);
}

function makeBar(close) { return { close, open: close, high: close, low: close, volume: 0 }; }

function fakeStrat(bars) {
  return {
    lastNClosedBars(_sym, _bt, _param, n) {
      return bars.slice(Math.max(0, bars.length - n));
    },
  };
}

const M1 = 60 * 1000000000;

// Climbing series: 100, 101.5, 103, ..., 110.5
const climbing = [];
for (let i = 0; i < 8; i++) climbing.push(makeBar(100 + i * 1.5));
const climbStrat = fakeStrat(climbing);

const fast = when(climbStrat, 1, BAR_TYPE_TIME, M1).ema(3);
const slow = when(climbStrat, 1, BAR_TYPE_TIME, M1).ema(6);
check('fast EMA ready', fast.isReady());
check('slow EMA ready', slow.isReady());
check('fast > slow on uptrend', fast.gt(slow).value());
check('fast < slow inverts', !fast.lt(slow).value());

const rsi = when(climbStrat, 1, BAR_TYPE_TIME, M1).rsi(3);
check('rsi ready', rsi.isReady());
check('rsi above 80 on monotone climb', rsi.value() > 80);

// Logical composition.
const aboveHundred = when(climbStrat, 1, BAR_TYPE_TIME, M1).close().gt(99);
check('and: cross && above', fast.gt(slow).and(aboveHundred).value());
check('or: cross || above', fast.gt(slow).or(aboveHundred).value());
check('not: !(cross down)', !fast.lt(slow).not().value() === false);

// Warmup short-circuits before there are enough bars.
const emptyStrat = fakeStrat([]);
const emptyFast = when(emptyStrat, 1, BAR_TYPE_TIME, M1).ema(3);
check('empty ring not ready', !emptyFast.isReady());

// Indicator-grid sugar (W3-T017).
const { grid } = require('../lib/composite');
const H1 = 3600 * 1000000000;
const g = grid(climbStrat, [1, 2], [M1, [BAR_TYPE_TIME, H1]]).ema(3);
check('grid size 2x2 = 4', g.size() === 4);
const btcM1 = g.get(1, BAR_TYPE_TIME, M1);
check('grid[(1, M1)] resolves', !!btcM1);
check('grid cell ready (uses fake ring)', btcM1.isReady());
const keys = g.keys();
check('grid keys length', keys.length === 4);
check('grid first key has symbol+param', keys[0].symbol === 1 && keys[0].param === M1);

console.log('node composite DSL test ok');
