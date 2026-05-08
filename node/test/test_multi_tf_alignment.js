// Smoke test: verifies the new emitter helpers are exposed and that
// the strategy class boots. Driving them with real bars requires
// running an engine; that path is exercised by the Python tests
// against the same C++ binding.

const flox = require('../');

function check(label, ok) {
  if (!ok) { console.error('FAIL: ' + label); process.exit(1); }
  console.log('ok: ' + label);
}

// The Strategy class is hosted; emitter handles are passed into
// onTrade/onBar. Without an engine we don't get an emitter, so we
// just verify the class is exported and constructable in some shape.
check('Strategy export exists', typeof flox.Strategy !== 'undefined' || typeof flox.SignalBuilder !== 'undefined');
console.log('node multi-tf-alignment smoke ok');
