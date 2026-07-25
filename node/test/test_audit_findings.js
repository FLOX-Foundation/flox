// Regression tests for defects that shipped behind green CI.
//
// Every one of these was reachable from the documented API and either threw
// on its only documented call form, or silently produced a wrong result. None
// were covered: the Node surface is exercised by ~23% of its callables, and
// index.d.ts is only checked against test_types.ts, which tsc type-checks but
// never runs.

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const flox = require('../index.js');

let passed = 0;
function check(cond, msg) {
  assert.ok(cond, msg);
  passed++;
}
function throws(fn, needle, msg) {
  let threw = null;
  try { fn(); } catch (e) { threw = e; }
  assert.ok(threw, msg + ' (expected a throw, got none)');
  assert.ok(String(threw.message).includes(needle),
            `${msg} (message was: ${threw.message})`);
  passed++;
}

// ── VenueStack static factories ───────────────────────────────────────
// index.d.ts says "Construct via static factory", but inside a NAPI
// StaticMethod `info.This()` IS the constructor, so `.constructor` returned
// the global `Function` and every factory threw "SyntaxError: Unexpected
// number" from `new Function(0, 1, 100000)`.
{
  const expected = {
    binanceUmFutures: 'binance_um_futures',
    bybitLinear: 'bybit_linear',
    okxSwap: 'okx_swap',
    deribit: 'deribit',
  };
  for (const [fn, venue] of Object.entries(expected)) {
    const stack = flox.VenueStack[fn](1, 100000);
    check(stack.venueName() === venue, `${fn}() -> ${venue}`);
  }
  for (const [name, venue] of [['binance', 'binance_um_futures'],
                               ['bybit', 'bybit_linear'],
                               ['okx_swap', 'okx_swap'],
                               ['deribit', 'deribit']]) {
    check(flox.VenueStack.fromVenue(name, 1, 100000).venueName() === venue,
          `fromVenue(${name})`);
  }
  throws(() => flox.VenueStack.fromVenue('nope', 1, 100000),
         'unknown venue', 'fromVenue rejects an unknown venue');
}

// ── Profile loaders reject typos ──────────────────────────────────────
// These were no-ops on an unknown name, so a typo'd venue produced a
// zero-fee, unthrottled backtest that looked perfectly healthy.
{
  const fee = new flox.FeeSchedule();
  fee.loadProfile('binance_um_futures');
  check(fee.feeFor(0, 10000, true) !== 0, 'a real profile sets a fee');
  throws(() => fee.loadProfile('TOTALLY_BOGUS'), 'unknown fee schedule profile',
         'FeeSchedule.loadProfile rejects a typo');

  throws(() => new flox.FundingSchedule().loadProfile('nope'),
         'unknown funding schedule profile', 'FundingSchedule.loadProfile rejects a typo');
  throws(() => new flox.RateLimitPolicy().loadProfile('nope'),
         'unknown rate limit policy profile', 'RateLimitPolicy.loadProfile rejects a typo');
}

// ── DataWriter.writeTrade accepts what index.d.ts declares ────────────
// Declared `number | bigint` timestamps and `Side` ("buy" | "sell"), but
// read Number for all three, so only an undeclared all-numeric call worked.
// bigint matters: 1765615835519000000 does not survive a double.
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'flox-writetrade-'));
  const w = new flox.DataWriter(dir);
  const ts = 1765615835519000000n;
  check(w.writeTrade(ts, ts, 100.5, 1.0, 7, 1, 'buy') === true,
        'writeTrade accepts bigint timestamps and a Side string');
  check(w.writeTrade(1765615835519, 1765615835519, 100.5, 1.0, 8, 1, 1) === true,
        'writeTrade still accepts the all-numeric form');
  throws(() => w.writeTrade(ts, ts, 1, 1, 9, 1, 'sideways'), 'unknown side',
         'writeTrade rejects an unknown side');
}

// ── Partitioner.byCalendar ────────────────────────────────────────────
// index.d.ts declared `0 | 1 | 2`, the addon read a string, and there was no
// else branch -- so every typo silently partitioned by hour (unit 0) instead
// of by the unit asked for.
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'flox-partition-'));
  const p = new flox.Partitioner(dir);
  for (const unit of ['hour', 'day', 'week', 'month']) {
    p.byCalendar(unit, 0);
    passed++;
  }
  throws(() => p.byCalendar('yearly', 0), 'unknown calendar unit',
         'byCalendar rejects an unknown unit instead of falling back to hour');
  throws(() => p.byCalendar(1, 0), 'expects a unit string',
         'byCalendar rejects the numeric form index.d.ts used to declare');
}

console.log(`${passed} check(s) passed`);
