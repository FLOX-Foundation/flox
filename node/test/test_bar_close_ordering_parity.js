// Cross-binding parity test for bar-close dispatch ordering.
// See python/tests/test_bar_close_ordering_parity.py — all four
// bindings must produce the same dispatch sequence on the same input.

const flox = require('..');

function check(label, ok) {
  if (!ok) { console.error('FAIL: ' + label); process.exit(1); }
  console.log('ok: ' + label);
}

const H4 = 4 * 60 * 60;
const H1 = 60 * 60;
const M5 = 5 * 60;
const NS = 1_000_000_000;
const H4_NS = H4 * NS;
const H1_NS = H1 * NS;
const M5_NS = M5 * NS;

function drive(secondsInOrder) {
  const r = new flox.BarDispatchRecorder();
  for (const s of secondsInOrder) r.addTimeIntervalSeconds(s);
  r.onTrade(1, 100.0, 0.1, 0);
  r.onTrade(1, 101.0, 0.1, H4_NS);
  r.finalize();
  const out = [];
  for (let i = 0; i < r.count(); i++) out.push(r.paramAt(i));
  return out;
}

{
  const params = drive([H4, H1, M5]);
  check('coarsest-first registration: first three are H4, H1, M5',
        params[0] === H4_NS && params[1] === H1_NS && params[2] === M5_NS);
}

{
  const params = drive([M5, H1, H4]);
  check('reverse registration: first three are M5, H1, H4',
        params[0] === M5_NS && params[1] === H1_NS && params[2] === H4_NS);
}

console.log('node bar-close ordering parity ok');
