'use strict';
/**
 * node/test/test_single_hook_mode.js -- the hook hosts have exactly one
 * mode, and a hook that is read inline can only ever be reached from the
 * JS thread.
 *
 * node/src/hooks.h carried a HookMode enum with a Sync and a Threaded
 * half, roughly 400 lines of ThreadSafeFunction plumbing behind the
 * second one, under a comment admitting that "nothing constructs a hook
 * host with HookMode::Threaded" -- every construction site in
 * node/src/strategy.h took the Sync default. Dead plumbing that looks
 * like a safety net is worse than none: `capabilitiesBridge` calls
 * `capabilities_fn.Call({})` under a comment saying it is "only safe to
 * call from the JS thread", and the only thing making that true is that
 * the threaded path was never taken.
 *
 * So the mode goes, and the invariant it pretended to uphold is enforced
 * where it is observable instead. index.d.ts already documents the four
 * hooks the engine reads inline -- setRiskManager, setKillSwitch,
 * setOrderValidator and setExecutor -- as "Sync only", and says of the
 * first "Throws if `threaded`". It did not throw: a threaded Runner
 * accepted all four and wired them into the LiveEngine, where a C++
 * consumer thread would call straight into V8. Refusing them at the
 * setter is what makes "only from the JS thread" a property of the code
 * rather than of the call sites nobody has written yet.
 *
 * Run from repo root:
 *   cd node && node test/test_single_hook_mode.js
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

// The four hooks index.d.ts marks "Sync only", with an object carrying
// the methods each one reads.
const INLINE_HOOKS = [
  ['setRiskManager', { allow() { return true; } }],
  ['setKillSwitch', { check() { return true; } }],
  ['setOrderValidator', { validate() { return true; } }],
  ['setExecutor', { submit() {}, cancel() {}, capabilities() { return {}; } }],
];

function makeRunner(threaded) {
  const reg = new flox.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const runner = new flox.Runner(reg, () => {}, threaded);
  return { runner, sym };
}

console.log('=== A threaded Runner refuses the hooks it would call off-thread ===');
for (const [setter, hook] of INLINE_HOOKS) {
  const { runner } = makeRunner(true);
  let err = null;
  try { runner[setter](hook); } catch (e) { err = e; }
  check(err !== null, `${setter} on a threaded Runner throws (got no throw)`);
  if (err !== null) {
    check(/sync/i.test(String(err.message)),
          `${setter} says why (message was "${err.message}")`);
  }
  try { runner.stop(); } catch (_) { /* never started */ }
}

console.log('\n=== Detaching is always allowed, on either kind of Runner ===');
for (const [setter] of INLINE_HOOKS) {
  const { runner } = makeRunner(true);
  let err = null;
  try { runner[setter](null); } catch (e) { err = e; }
  check(err === null,
        `${setter}(null) on a threaded Runner is accepted` + (err ? ` (threw ${err.message})` : ''));
  try { runner.stop(); } catch (_) { /* never started */ }
}

console.log('\n=== A sync Runner still takes every one of them ===');
for (const [setter, hook] of INLINE_HOOKS) {
  const { runner } = makeRunner(false);
  let err = null;
  try { runner[setter](hook); } catch (e) { err = e; }
  check(err === null, `${setter} on a sync Runner is accepted` + (err ? ` (threw ${err.message})` : ''));
  try { runner.stop(); } catch (_) { /* never started */ }
}

console.log('\n=== BacktestRunner is sync by construction and takes an executor ===');
{
  const reg = new flox.SymbolRegistry();
  reg.addSymbol('test', 'BTC', 0.01);
  const btr = new flox.BacktestRunner(reg, 0.0, 10000.0);
  let err = null;
  // Which OS thread called capabilities() is not observable from JS --
  // there is no thread id to read. What is observable is the set of
  // runners that can hold an executor at all, and after the refusal
  // above that set contains only runners whose callbacks run inline on
  // the JS thread: a sync Runner and this one.
  try {
    btr.setExecutor({ submit() {}, cancel() {}, capabilities() { return {}; } });
  } catch (e) { err = e; }
  check(err === null, `BacktestRunner.setExecutor is accepted` + (err ? ` (threw ${err.message})` : ''));
}

console.log('\n=== The dead mode is gone from the source ===');
{
  // A source check rather than a runtime one: an enum nothing constructs
  // has no runtime shadow to test. This is the whole point -- the
  // Threaded half was invisible from JS precisely because it was dead.
  const hooksPath = path.join(__dirname, '..', 'src', 'hooks.h');
  check(fs.existsSync(hooksPath),
        'node/src/hooks.h is readable (this check only runs from a repo checkout)');
  if (fs.existsSync(hooksPath)) {
    const src = fs.readFileSync(hooksPath, 'utf8');
    const occurrences = (src.match(/HookMode/g) || []).length;
    check(occurrences === 0,
          `node/src/hooks.h has no HookMode left (found ${occurrences} mention(s))`);
    check(!/enum class HookMode/.test(src),
          'node/src/hooks.h declares no HookMode enum');
  }

  const strategyPath = path.join(__dirname, '..', 'src', 'strategy.h');
  check(fs.existsSync(strategyPath), 'node/src/strategy.h is readable');
  if (fs.existsSync(strategyPath)) {
    const src = fs.readFileSync(strategyPath, 'utf8');
    const occurrences = (src.match(/HookMode/g) || []).length;
    check(occurrences === 0,
          `node/src/strategy.h has no HookMode left (found ${occurrences} mention(s))`);
  }
}

console.log('\n=== index.d.ts still documents the four as sync only ===');
{
  const dts = fs.readFileSync(path.join(__dirname, '..', 'index.d.ts'), 'utf8');
  const runner = dts.match(/export class Runner\s*\{([\s\S]*?)\n\}/);
  check(runner !== null, 'index.d.ts declares class Runner');
  if (runner !== null) {
    for (const [setter] of INLINE_HOOKS) {
      const idx = runner[1].indexOf(`${setter}(`);
      check(idx >= 0, `index.d.ts declares Runner.${setter}`);
      if (idx < 0) { continue; }
      // The doc comment sits immediately above the member.
      const preceding = runner[1].slice(Math.max(0, idx - 400), idx);
      const lastComment = preceding.lastIndexOf('/**');
      check(lastComment >= 0 && /sync only/i.test(preceding.slice(lastComment)),
            `Runner.${setter} is documented "Sync only"`);
      check(lastComment >= 0 && /throws/i.test(preceding.slice(lastComment)),
            `Runner.${setter} documents that it throws on a threaded Runner`);
    }
  }
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
