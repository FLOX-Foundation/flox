'use strict';
/**
 * node/test/test_bar_timestamp_bigint.js -- every bar timestamp the Node
 * addon emits is a BigInt nanosecond value, and survives a reading past
 * 2^53 unchanged.
 *
 * docs/explanation/javascript-value-boundary.md names `bar.startTimeNs`
 * and `bar.endTimeNs` among the fields that cross into JavaScript as a
 * BigInt, and says of the write side that "every binding that takes a
 * nanosecond argument accepts a BigInt or a Number". Bars in the Node
 * addon did neither: node/src/strategy.h built both fields with
 * Napi::Number::New(env, static_cast<double>(...)) and
 * node/src/aggregators.h with a plain (double) cast -- the first of them
 * 32 lines below the trade timestamp, which is a Napi::BigInt under a
 * comment reading "a real ns timestamp does not survive a double".
 *
 * A JS Number is a double, exact only to 2^53. At the ~1.76e18 magnitude
 * of a wall-clock nanosecond reading the gap between two representable
 * doubles is 256 ns, so every tick / volume / range / renko close came
 * back quantised, and index.d.ts typed the fields `number` while
 * TradeData.timestampNs next to them was already `bigint`.
 *
 * The aggregators additionally took their timestamp column as a
 * Float64Array, which cannot carry an exact nanosecond reading at all.
 * Passing a BigInt64Array instead was an unchecked .As<Napi::Float64Array>()
 * cast that reinterpreted the int64 bit patterns as doubles and produced
 * silent garbage, so the exact column has to be accepted explicitly.
 * BacktestRunner.runBars / .runOhlcv already take BigInt64Array for their
 * timestamp columns, and tape_aggregators.h already emits `startNs` as a
 * Napi::BigInt -- this brings the rest of the bar surface in line.
 *
 * Run from repo root:
 *   cd node && node test/test_bar_timestamp_bigint.js
 */

const fs = require('fs');
const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

// The reading from the review of node/src/data_ops.h: not representable
// as a double, so a value that made the Number round trip comes back as
// ...064 rather than ...000.
const NS = 1765615835519000000n;
const NS_AS_DOUBLE = 1765615835519000064n;
const SECOND_NS = 1000000000n;

// Read a value back at full width whatever its JS type, so the type
// check and the exactness check fail independently and each says why.
function exact(v) {
  return typeof v === 'bigint' ? v : BigInt(v);
}

// JSON.stringify refuses a BigInt, and every object printed in this file
// may carry one.
function describe(v) {
  return JSON.stringify(v, (_k, x) => (typeof x === 'bigint' ? `${x}n` : x));
}

function checkNsField(label, value, expected) {
  check(typeof value === 'bigint', `${label} is a bigint (got ${typeof value})`);
  check(exact(value) === expected,
        `${label} round-trips exactly (got ${exact(value)}, want ${expected})`);
}

// ── Batch aggregators ─────────────────────────────────────────────────
// Six trades one second apart, one dollar apart, so every policy closes
// at least one bar. The first bar always opens on the first trade, so its
// startTimeNs is the input reading verbatim.

const TRADE_COUNT = 6;

function bigIntTimestamps() {
  const ts = new BigInt64Array(TRADE_COUNT);
  const px = new Float64Array(TRADE_COUNT);
  const qty = new Float64Array(TRADE_COUNT);
  const ib = new Uint8Array(TRADE_COUNT);
  for (let i = 0; i < TRADE_COUNT; i++) {
    ts[i] = NS + BigInt(i) * SECOND_NS;
    px[i] = 100 + i;
    qty[i] = 1;
    ib[i] = 1;
  }
  return { ts, px, qty, ib };
}

function doubleTimestamps() {
  const ts = new Float64Array(TRADE_COUNT);
  const px = new Float64Array(TRADE_COUNT);
  const qty = new Float64Array(TRADE_COUNT);
  const ib = new Uint8Array(TRADE_COUNT);
  for (let i = 0; i < TRADE_COUNT; i++) {
    ts[i] = Number(NS + BigInt(i) * SECOND_NS);
    px[i] = 100 + i;
    qty[i] = 1;
    ib[i] = 1;
  }
  return { ts, px, qty, ib };
}

// The four policies the audit named plus the two time-bucketed ones. The
// four open their first bar on the first trade; aggregateTimeBars and
// aggregateHeikinAshiBars floor the start to an interval boundary, so
// only their type is pinned here.
const OPENS_ON_FIRST_TRADE = [
  ['aggregateTickBars', 1],
  ['aggregateVolumeBars', 0.5],
  ['aggregateRangeBars', 0.5],
  ['aggregateRenkoBars', 0.5],
];
const TIME_BUCKETED = [
  ['aggregateTimeBars', 1],
  ['aggregateHeikinAshiBars', 1],
];

console.log('=== The premise: a Number cannot hold this reading ===');
check(BigInt(Number(NS)) === NS_AS_DOUBLE,
      `${NS} through a Number becomes ${BigInt(Number(NS))} (64 ns off)`);

console.log('\n=== Batch aggregators emit bar timestamps as BigInt ===');
for (const [fn, param] of OPENS_ON_FIRST_TRADE.concat(TIME_BUCKETED)) {
  const { ts, px, qty, ib } = bigIntTimestamps();
  let bars;
  try {
    bars = flox[fn](ts, px, qty, ib, param);
  } catch (e) {
    check(false, `${fn} accepts a BigInt64Array timestamp column (threw ${e.constructor.name}: ${e.message})`);
    continue;
  }
  check(Array.isArray(bars) && bars.length > 0, `${fn} produced at least one bar (got ${bars && bars.length})`);
  if (!bars || bars.length === 0) { continue; }
  check(typeof bars[0].startTimeNs === 'bigint',
        `${fn} bar.startTimeNs is a bigint (got ${typeof bars[0].startTimeNs})`);
  check(typeof bars[0].endTimeNs === 'bigint',
        `${fn} bar.endTimeNs is a bigint (got ${typeof bars[0].endTimeNs})`);
}

console.log('\n=== Bar timestamps past 2^53 survive the aggregators exactly ===');
for (const [fn, param] of OPENS_ON_FIRST_TRADE) {
  const { ts, px, qty, ib } = bigIntTimestamps();
  let bars;
  try {
    bars = flox[fn](ts, px, qty, ib, param);
  } catch (e) {
    check(false, `${fn} accepts a BigInt64Array timestamp column (threw ${e.constructor.name}: ${e.message})`);
    continue;
  }
  if (!bars || bars.length === 0) { check(false, `${fn} produced a bar to inspect`); continue; }
  checkNsField(`${fn} bar[0].startTimeNs`, bars[0].startTimeNs, NS);
}

console.log('\n=== A Float64Array timestamp column still works ===');
for (const [fn, param] of OPENS_ON_FIRST_TRADE.concat(TIME_BUCKETED)) {
  const { ts, px, qty, ib } = doubleTimestamps();
  let bars;
  try {
    bars = flox[fn](ts, px, qty, ib, param);
  } catch (e) {
    check(false, `${fn} still accepts a Float64Array timestamp column (threw ${e.message})`);
    continue;
  }
  check(Array.isArray(bars) && bars.length > 0,
        `${fn} still produces bars from a Float64Array column (got ${bars && bars.length})`);
}

// ── The bar handed to a strategy's onBar ──────────────────────────────

console.log('\n=== The bar fed to onBar carries BigInt timestamps ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.BacktestRunner(reg, 0.0, 10000.0);

  const seen = [];
  runner.setStrategy({ symbols: [sym], onBar(_ctx, bar) { seen.push(bar); } });

  const startNs = NS;
  const endNs = NS + SECOND_NS - 1n;
  runner.runBars(
    new BigInt64Array([startNs]),
    new BigInt64Array([endNs]),
    new Float64Array([100.0]),
    new Float64Array([101.0]),
    new Float64Array([99.0]),
    new Float64Array([100.5]),
    new Float64Array([10.0]),
    'BTC');

  check(seen.length === 1, `exactly one bar delivered (got ${seen.length})`);
  if (seen.length === 1) {
    checkNsField('onBar bar.startTimeNs', seen[0].startTimeNs, startNs);
    checkNsField('onBar bar.endTimeNs', seen[0].endTimeNs, endNs);
    // barTypeParam is an interval, not a clock reading, and stays a
    // Number -- see the "What did not change" section of the boundary doc.
    check(typeof seen[0].barTypeParam === 'number',
          `onBar bar.barTypeParam stays a number (got ${typeof seen[0].barTypeParam})`);
  }
}

console.log('\n=== A bar goes back in the way it came out ===');
{
  // Runner.onBar reads its bar fields through an `IsNumber()` guard that
  // falls back to the default, so a BigInt startTimeNs did not throw and
  // did not arrive -- it silently became 0. Once bars come out carrying
  // BigInts, handing one straight back has to work.
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.Runner(reg, () => {}, false);
  const seen = [];
  runner.addStrategy({ symbols: [sym], onBar(_ctx, bar) { seen.push(bar); } });
  runner.start();
  const startNs = NS;
  const endNs = NS + SECOND_NS - 1n;
  runner.onBar(Number(sym), {
    open: 100, high: 101, low: 99, close: 100.5, volume: 1,
    startTimeNs: startNs, endTimeNs: endNs,
  });
  runner.stop();

  check(seen.length === 1, `Runner.onBar delivered one bar (got ${seen.length})`);
  if (seen.length === 1) {
    checkNsField('Runner.onBar round-trip startTimeNs', seen[0].startTimeNs, startNs);
    checkNsField('Runner.onBar round-trip endTimeNs', seen[0].endTimeNs, endNs);
  }
}

console.log('\n=== Runner.onBar still takes Number bar timestamps ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.Runner(reg, () => {}, false);
  const seen = [];
  runner.addStrategy({ symbols: [sym], onBar(_ctx, bar) { seen.push(bar); } });
  runner.start();
  runner.onBar(Number(sym), {
    open: 100, high: 101, low: 99, close: 100.5, volume: 1,
    startTimeNs: 60000000000, endTimeNs: 119999999999,
  });
  runner.stop();
  check(seen.length === 1, `Runner.onBar delivered one bar from Numbers (got ${seen.length})`);
  if (seen.length === 1) {
    check(exact(seen[0].startTimeNs) === 60000000000n,
          `a Number startTimeNs under 2^53 survives (got ${exact(seen[0].startTimeNs)})`);
    check(exact(seen[0].endTimeNs) === 119999999999n,
          `a Number endTimeNs under 2^53 survives (got ${exact(seen[0].endTimeNs)})`);
  }
}

console.log('\n=== The multi-timeframe bar ring hands back BigInt readings ===');
{
  // emit.lastClosedBar() / emit.lastNClosedBars() read the per-(symbol,
  // timeframe) ring the strategy base class fills on every closed bar.
  // They build their own bar object -- a different builder from the one
  // feeding onBar -- and nothing drove either accessor at runtime, so
  // only the ClosedBar text in index.d.ts stood behind them.
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.Runner(reg, () => {}, false);

  const TF_NS = 1000000000;  // a one-second time bar
  const BAR_TYPE = 0;        // BarData.barType 0 = Time
  const seen = [];
  runner.addStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) {
      seen.push({
        last: emit.lastClosedBar(Number(sym), BAR_TYPE, TF_NS),
        all: emit.lastNClosedBars(Number(sym), BAR_TYPE, TF_NS, 8),
      });
    },
  });
  runner.start();
  const firstStart = NS;
  const secondStart = NS + SECOND_NS;
  for (const startNs of [firstStart, secondStart]) {
    runner.onBar(Number(sym), {
      open: 100, high: 101, low: 99, close: 100.5, volume: 1,
      barType: BAR_TYPE, barTypeParam: TF_NS,
      startTimeNs: startNs, endTimeNs: startNs + SECOND_NS - 1n,
    });
  }
  runner.stop();

  check(seen.length === 2, `both bars reached the strategy (got ${seen.length})`);
  if (seen.length === 2) {
    const last = seen[1].last;
    check(last !== null && typeof last === 'object',
          `lastClosedBar() returned a bar (got ${describe(last)})`);
    if (last) {
      checkNsField('lastClosedBar().startNs', last.startNs, secondStart);
      checkNsField('lastClosedBar().endNs', last.endNs, secondStart + SECOND_NS - 1n);
      check(typeof last.close === 'number',
            `lastClosedBar().close stays a number (got ${typeof last.close})`);
    }

    const all = seen[1].all;
    check(Array.isArray(all) && all.length === 2,
          `lastNClosedBars() returned both bars oldest first (got ${all && all.length})`);
    if (Array.isArray(all) && all.length === 2) {
      checkNsField('lastNClosedBars()[0].startNs', all[0].startNs, firstStart);
      checkNsField('lastNClosedBars()[0].endNs', all[0].endNs, firstStart + SECOND_NS - 1n);
      checkNsField('lastNClosedBars()[1].startNs', all[1].startNs, secondStart);
      checkNsField('lastNClosedBars()[1].endNs', all[1].endNs, secondStart + SECOND_NS - 1n);
    }

    // The first callback saw only its own bar.
    check(Array.isArray(seen[0].all) && seen[0].all.length === 1,
          `the first callback saw one bar in the ring (got ${seen[0].all && seen[0].all.length})`);
  }
}

console.log('\n=== An empty ring reads back as null, not as a zeroed bar ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.Runner(reg, () => {}, false);
  const seen = [];
  runner.addStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) {
      // A timeframe nothing has closed into.
      seen.push({
        last: emit.lastClosedBar(Number(sym), 0, 60000000000),
        all: emit.lastNClosedBars(Number(sym), 0, 60000000000, 4),
      });
    },
  });
  runner.start();
  runner.onBar(Number(sym), {
    open: 100, high: 101, low: 99, close: 100.5, volume: 1,
    barType: 0, barTypeParam: 1000000000,
    startTimeNs: NS, endTimeNs: NS + SECOND_NS - 1n,
  });
  runner.stop();
  check(seen.length === 1 && seen[0].last === null,
        `an unfilled timeframe reads back null (got ${describe(seen[0] && seen[0].last)})`);
  check(seen.length === 1 && Array.isArray(seen[0].all) && seen[0].all.length === 0,
        `an unfilled timeframe yields no bars (got ${seen[0] && seen[0].all && seen[0].all.length})`);
}

// ── index.d.ts agrees with what the addon emits ───────────────────────

console.log('\n=== index.d.ts types every bar timestamp bigint ===');
{
  const dts = fs.readFileSync(path.join(__dirname, '..', 'index.d.ts'), 'utf8');

  function interfaceBody(name) {
    const m = dts.match(new RegExp(`export interface ${name}\\s*\\{([\\s\\S]*?)\\n\\}`));
    return m ? m[1] : null;
  }

  const fields = [
    ['BarData', ['startTimeNs', 'endTimeNs']],
    ['AggregatedBar', ['startTimeNs', 'endTimeNs']],
    ['ClosedBar', ['startNs', 'endNs']],
  ];
  for (const [iface, names] of fields) {
    const body = interfaceBody(iface);
    check(body !== null, `index.d.ts declares interface ${iface}`);
    if (body === null) { continue; }
    for (const field of names) {
      const declared = body.match(new RegExp(`^\\s*${field}:\\s*([^;]+);`, 'm'));
      check(declared !== null, `${iface}.${field} is declared`);
      if (declared) {
        check(declared[1].trim() === 'bigint',
              `${iface}.${field} is typed bigint (declared "${declared[1].trim()}")`);
      }
    }
  }

  // The write side of the same contract: the aggregator timestamp column
  // has to admit the exact type.
  const aggregators = OPENS_ON_FIRST_TRADE.concat(TIME_BUCKETED).map(([fn]) => fn);
  for (const fn of aggregators) {
    const m = dts.match(new RegExp(`export function ${fn}\\(([\\s\\S]*?)\\):`));
    check(m !== null, `index.d.ts declares ${fn}`);
    if (!m) { continue; }
    check(/timestamps:[^,]*BigInt64Array/.test(m[1]),
          `${fn} declares a BigInt64Array timestamp column (declared "${(m[1].match(/timestamps:[^,]*/) || ['<missing>'])[0].trim()}")`);
    check(/timestamps:[^,]*Float64Array/.test(m[1]),
          `${fn} still declares the Float64Array timestamp column`);
  }
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
