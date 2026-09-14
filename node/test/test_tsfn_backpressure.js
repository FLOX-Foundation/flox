'use strict';
/**
 * node/test/test_tsfn_backpressure.js — the ThreadSafeFunction queue behind
 * a threaded Runner must be bounded, and a rejected call must not leak its
 * payload.
 *
 * Before the fix: `Napi::ThreadSafeFunction::New(..., /*max_queue_size=*\/0, 1)`
 * meant "unbounded", and every `NonBlockingCall` site ignored the returned
 * status. Publishing without draining the event loop queued synchronously
 * with zero backpressure: measured 38 MB RSS growth at 200k queued events,
 * and a hard SIGABRT (exit 134, the queue's own allocator giving up) at 2M.
 *
 * After the fix: the queue is capped (kTsfnMaxQueueSize in
 * node/src/tsfn_util.h) and every call site frees its heap payload when the
 * call is rejected, so publishing far past the cap degrades to bounded
 * memory and bounded delivery instead of unbounded growth or a crash.
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

// AddressSanitizer/UndefinedBehaviorSanitizer redzones and shadow memory
// multiply RSS for every allocation, independent of whether the queue is
// actually bounded -- the numeric threshold below is only meaningful
// against a plain build. Detect a sanitizer-instrumented run (see the
// DYLD_INSERT_LIBRARIES / *SAN_OPTIONS the sanitizer run script sets) and
// report the number without failing on it; the functional checks below
// (bounded delivery, no crash) still apply either way.
const underSanitizer = /clang_rt\.(asan|ubsan|tsan)/i.test(process.env.DYLD_INSERT_LIBRARIES || '')
  || !!process.env.ASAN_OPTIONS || !!process.env.UBSAN_OPTIONS || !!process.env.TSAN_OPTIONS;

const reg = new flox.SymbolRegistry();
const sym = Number(reg.addSymbol('bin', 'BTCUSDT', 0.01));

let seen = 0;
const runner = new flox.Runner(reg, () => {}, true);
runner.addStrategy({ symbols: [sym], onTrade() { seen++; } });
runner.start();

// The finding's repro published 2,000,000 events with the loop never
// yielding, which used to abort the process. Reproduce the same shape.
const N = 2_000_000;
const rss0 = process.memoryUsage().rss;
for (let i = 0; i < N; i++) {
  runner.onTrade(sym, 100, 1, true, 1000 + i);
}
const rss1 = process.memoryUsage().rss;
const growthMb = (rss1 - rss0) / 1048576;
console.log(`published ${N} events synchronously; RSS growth = ${growthMb.toFixed(1)} MB`);

// Unbounded, this measured ~376 MB growth (52 -> 428 MB) before aborting.
// Bounded at 65536 slots and ~200 bytes/event, worst case is ~13 MB; allow
// generous headroom for V8/allocator overhead without re-permitting
// unbounded growth.
if (underSanitizer) {
  console.log(`(running under a sanitizer -- RSS threshold not evaluated, growth reported for reference only)`);
} else {
  check(growthMb < 100, `RSS growth is bounded, not proportional to ${N} events (got ${growthMb.toFixed(1)} MB)`);
}

setTimeout(() => {
  console.log(`after event-loop drain: delivered to JS = ${seen}`);
  // Most of the 2M calls were rejected by the full queue (by design --
  // that is the backpressure) and their payload was freed immediately
  // rather than queued or leaked, so delivered count is on the order of
  // the queue capacity, not the publish count.
  check(seen > 0, 'at least some events made it through the bounded queue');
  check(seen < N, 'the bounded queue actually rejected the excess instead of delivering all 2,000,000');
  runner.stop();

  console.log(`${passed} passed, ${failed} failed`);
  process.exitCode = failed > 0 ? 1 : 0;

  // runner.stop() releases the channel, so the loop drains and the process
  // ends on its own. The unref'd timer fires only if it does not.
  const watchdog = setTimeout(() => {
    console.error('test_tsfn_backpressure: the event loop is still alive 10s after stop()');
    process.exit(3);
  }, 10000);
  watchdog.unref();
}, 3000);
