'use strict';
/**
 * node/test/test_tsfn_exception_leak.js — a strategy callback that throws
 * in threaded mode must not leak the dispatch payload.
 *
 * Before the fix: `self->on_trade_fn.Call(...)` throwing a JS exception
 * (surfaced as a C++ exception under NODE_ADDON_API_CPP_EXCEPTIONS_ALL)
 * unwound straight past the `delete d;` at the end of callOnTrade and its
 * siblings, so every strategy exception leaked its *CallData. In threaded
 * mode the exception itself is also silently swallowed by Node's own
 * uncaught-Node-API-callback-exception policy (a separate, documented
 * asymmetry with sync mode -- not something this addon can change), so
 * nothing about a throwing threaded strategy was otherwise observable
 * except the leak.
 *
 * After the fix: callOnTrade (and the other TSFN consumer callbacks)
 * unique_ptr-guard the payload, so it is freed whether or not the JS call
 * throws.
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

// Node warns once per dispatch that the strategy's exception was
// swallowed (a separate, documented asymmetry with sync mode -- see
// docs/reference/node/strategy.md). That is expected and orthogonal to
// what this test measures; silence it so CI output stays readable.
process.on('warning', () => {});

// See test_tsfn_backpressure.js -- ASan/UBSan/TSan redzones and shadow
// memory make RSS not comparable to a plain build.
const underSanitizer = /clang_rt\.(asan|ubsan|tsan)/i.test(process.env.DYLD_INSERT_LIBRARIES || '')
  || !!process.env.ASAN_OPTIONS || !!process.env.UBSAN_OPTIONS || !!process.env.TSAN_OPTIONS;

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const reg = new flox.SymbolRegistry();
const sym = Number(reg.addSymbol('bin', 'BTCUSDT', 0.01));

const runner = new flox.Runner(reg, () => {}, true);
runner.addStrategy({
  symbols: [sym],
  onTrade() {
    throw new Error('boom from strategy');
  },
});
runner.start();

// Every one of these dispatches will throw inside callOnTrade. Publish
// enough that an unfreed TradeCallData per event would show up as
// unmistakable RSS growth well past a single event-loop drain.
const N = 500_000;
for (let i = 0; i < N; i++) {
  runner.onTrade(sym, 100, 1, true, 1000 + i);
}

// Drain the event loop repeatedly (bounded queue means this needs more
// than one macrotask turn to work through all N).
let drains = 0;
function drainStep() {
  drains += 1;
  if (drains < 40) {
    setTimeout(drainStep, 50);
    return;
  }
  if (global.gc) {
    global.gc();
  }
  const rssAfter = process.memoryUsage().rss;
  console.log(`RSS after publishing+draining ${N} throwing dispatches: ${(rssAfter / 1048576).toFixed(1)} MB`);
  if (underSanitizer) {
    console.log(`(running under a sanitizer -- RSS threshold not evaluated, reported for reference only)`);
  } else {
    // ~200 bytes/event leaked at N=500,000 would be ~95 MB; a few live
    // buffers plus V8/allocator overhead should stay well under that.
    check(rssAfter / 1048576 < 150, `RSS stays bounded instead of leaking one TradeCallData per throw (got ${(rssAfter / 1048576).toFixed(1)} MB)`);
  }
  // Reaching this line at all means N throwing dispatches did not crash
  // the process or corrupt the runner's state.
  check(true, `process survived ${N} throwing threaded dispatches without crashing`);
  runner.stop();
  console.log(`${passed} passed, ${failed} failed`);
  process.exitCode = failed > 0 ? 1 : 0;

  // runner.stop() releases the channel, so the loop drains and the process
  // ends on its own. The unref'd timer fires only if it does not.
  const watchdog = setTimeout(() => {
    console.error('test_tsfn_exception_leak: the event loop is still alive 10s after stop()');
    process.exit(3);
  }, 10000);
  watchdog.unref();
}
drainStep();
