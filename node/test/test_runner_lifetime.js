'use strict';
/**
 * node/test/test_runner_lifetime.js — who owns a threaded Runner's
 * ThreadSafeFunction, and for how long.
 *
 * Three behaviours, one ownership rule (node/src/tsfn_util.h):
 *
 *  1. A channel is opened by start() and released by stop(), so a threaded
 *     Runner that has been stopped no longer holds Node's event loop open.
 *     Before that rule, the TSFN was created in the constructor and released
 *     only from the ObjectWrap destructor — which waits for GC, which waits
 *     for the loop to go idle, which the TSFN itself prevents. A script that
 *     constructed a threaded Runner never exited on its own.
 *
 *  2. Releasing a channel does not empty it. Items still queued run on a
 *     later tick, after the owner is gone, so each payload carries a liveness
 *     flag the owner retires in its destructor. Without it, an abandoned
 *     Runner whose queue was not empty crashed the process as soon as GC
 *     collected it.
 *
 *  3. replaceStrategy writes nine FunctionReferences on the JS thread. The
 *     bus thread reads a published bitmask instead of the references, so a
 *     swap on a live engine cannot be observed mid-write. This file checks
 *     the functional half; the absence of the race itself is a
 *     ThreadSanitizer result, not something a JS assertion can show.
 *
 * The child processes below are the point of cases 1 and 2: whether a process
 * exits is not observable from inside itself.
 */

const path = require('path');
const { spawnSync } = require('child_process');
const flox = require(path.join(__dirname, '..'));

const ADDON = path.join(__dirname, '..');
let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

// A sanitizer-instrumented addon is loaded into the child through dlopen, and
// the runtime that has to be in place before that only arrives through
// DYLD_INSERT_LIBRARIES / LD_PRELOAD -- which the loader strips from the
// parent's own environment once it has applied it, so a child never inherits
// it and aborts with "Interceptors are not working". The three child cases
// below are about the event loop, which instrumentation does not change, so
// they are reported as skipped rather than failed on such a run. The
// in-process case still runs. Same detection as test_tsfn_backpressure.js.
const underSanitizer = /clang_rt\.(asan|ubsan|tsan)/i.test(process.env.DYLD_INSERT_LIBRARIES || '')
  || !!process.env.ASAN_OPTIONS || !!process.env.UBSAN_OPTIONS || !!process.env.TSAN_OPTIONS;

function skip(msg) {
  console.log(`  skip  ${msg} (sanitizer run: the child cannot load the runtime early enough)`);
}

// Run `source` in a fresh node process and report how it ended. A child that
// is still running when the timeout expires comes back with status null and
// signal SIGTERM, which is the shape the unfixed lifetime produced.
function runChild(source, extraArgs = []) {
  const started = Date.now();
  const r = spawnSync(process.execPath, [...extraArgs, '-e', source], {
    timeout: 20000,
    encoding: 'utf8',
  });
  return {
    status: r.status,
    signal: r.signal,
    ms: Date.now() - started,
    stdout: r.stdout || '',
    stderr: r.stderr || '',
  };
}

const PRELUDE = `
const flox = require(${JSON.stringify(ADDON)});
const reg = new flox.SymbolRegistry();
const sym = Number(reg.addSymbol('bin', 'BTCUSDT', 0.01));
`;

console.log('=== a stopped threaded runner releases the event loop ===');
if (underSanitizer) { skip('stopped runner releases the event loop'); } else {
  const r = runChild(PRELUDE + `
const runner = new flox.Runner(reg, () => {}, true);
runner.addStrategy({ symbols: [sym], onTrade() {} });
runner.start();
runner.onTrade(sym, 100, 1, true, 1000);
runner.stop();
console.log('END');
process.exitCode = 0;
`);
  check(r.signal === null, `child was not killed on the timeout (signal=${r.signal})`);
  check(r.status === 0, `child exited cleanly (status=${r.status})`);
  check(r.stdout.includes('END'), 'child reached the end of the script');
  console.log(`  (exit took ${r.ms} ms)`);
}

console.log('=== constructing a threaded runner holds nothing open ===');
if (underSanitizer) { skip('bare construction holds nothing open'); } else {
  const r = runChild(PRELUDE + `
const runner = new flox.Runner(reg, () => {}, true);
void runner;
console.log('END');
process.exitCode = 0;
`);
  check(r.signal === null, `child was not killed on the timeout (signal=${r.signal})`);
  check(r.status === 0, `child exited cleanly (status=${r.status})`);
}

console.log('=== an abandoned runner with a queued backlog does not crash ===');
if (underSanitizer) { skip('abandoned runner with a queued backlog'); } else {
  // The runner goes out of scope with items still in its channel. GC runs the
  // destructor, which retires the liveness flag before releasing the channel,
  // so the queued items free their payload and return instead of calling back
  // into freed storage. Unfixed, this aborted with a FATAL ERROR raised from
  // callOnTrade.
  const r = runChild(PRELUDE + `
let seen = 0;
(function () {
  const runner = new flox.Runner(reg, () => {}, true);
  runner.addStrategy({ symbols: [sym], onTrade() { seen++; } });
  runner.start();
  for (let i = 0; i < 20000; i++) runner.onTrade(sym, 100, 1, true, 1000 + i);
  runner.stop();
})();
global.gc();
global.gc();
setTimeout(() => { console.log('END delivered=' + seen); process.exitCode = 0; }, 500);
`, ['--expose-gc']);
  check(r.signal === null, `child was not killed on the timeout (signal=${r.signal})`);
  check(r.status === 0, `child exited cleanly (status=${r.status}), stderr: ${r.stderr.split('\n')[0]}`);
  check(!/FATAL ERROR/.test(r.stderr), 'no fatal error from a queued callback');
  check(r.stdout.includes('END'), 'child reached the end of the script');
}

console.log('=== replaceStrategy on a running engine keeps delivering ===');
{
  const reg = new flox.SymbolRegistry();
  const sym = Number(reg.addSymbol('bin', 'ETHUSDT', 0.01));
  const runner = new flox.Runner(reg, () => {}, true);

  const seen = [];
  const make = (label) => ({ symbols: [sym], onTrade() { seen.push(label); } });

  runner.addStrategy(make('A'));
  runner.start();
  for (let i = 0; i < 200; i++) runner.onTrade(sym, 100, 1, true, 1000 + i);
  runner.replaceStrategy(0, make('B'));
  for (let i = 0; i < 200; i++) runner.onTrade(sym, 101, 1, true, 2000 + i);

  setTimeout(() => {
    runner.stop();
    setTimeout(() => {
      check(seen.length > 0, `events reached JS across the swap (got ${seen.length})`);
      check(seen.includes('B'), 'the replacement strategy received events');
      finish();
    }, 200);
  }, 500);
}

function finish() {
  console.log(`\n${passed} passed, ${failed} failed`);
  process.exitCode = failed > 0 ? 1 : 0;

  // An unref'd timer does not keep the loop alive on its own, so in a healthy
  // run it never fires. It fires only if something else is still holding the
  // loop open — which is the defect this file exists for, and which used to
  // sit in CI until GitHub's six-hour default cut it off. Turn that into a
  // ten-second failure with its own exit code.
  const watchdog = setTimeout(() => {
    console.error('test_runner_lifetime: the event loop is still alive 10s after the last assertion');
    process.exit(3);
  }, 10000);
  watchdog.unref();
}
