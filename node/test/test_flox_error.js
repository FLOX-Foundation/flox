'use strict';
/**
 * node/test/test_flox_error.js — verify that FloxError thrown from C++
 * surfaces in Node.js with .code / .helpUrl set, and is `instanceof Error`.
 *
 * Run from repo root:
 *   cd node && node test/test_flox_error.js
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(condition, msg) {
  if (condition) {
    console.log(`  ok  ${msg}`);
    passed++;
  } else {
    console.error(`  FAIL  ${msg}`);
    failed++;
  }
}

// ── E_IO_001: file not found ────────────────────────────────────────

function testIoNotFound() {
  console.log('testIoNotFound');
  const eng = new flox.Engine(10000, 0.001);
  try {
    eng.loadCsv('/this/path/does/not/exist.csv', 'BTC');
    check(false, 'expected loadCsv to throw');
  } catch (e) {
    check(e instanceof Error, 'error is instanceof Error');
    check(e.code === 'E_IO_001', `error.code === 'E_IO_001' (got ${e.code})`);
    check(typeof e.helpUrl === 'string' && e.helpUrl.length > 0,
          `error.helpUrl is non-empty string`);
    check(e.helpUrl.includes('E_IO_001'),
          `error.helpUrl mentions code (got ${e.helpUrl})`);
    check(e.message.includes('does/not/exist.csv'),
          `error.message names the offending path`);
  }
}

// ── E_SYM_001: resample with unknown source symbol ────────────────────

function testResampleUnknownSymbol() {
  console.log('testResampleUnknownSymbol');
  const eng = new flox.Engine(10000, 0.001);
  try {
    eng.resample('NEVER_LOADED', 'TARGET', '1m');
    check(false, 'expected resample to throw for unloaded source');
  } catch (e) {
    check(e instanceof Error, 'error is instanceof Error');
    check(e.code === 'E_SYM_001', `error.code === 'E_SYM_001' (got ${e.code})`);
    check(typeof e.helpUrl === 'string' && e.helpUrl.includes('E_SYM_001'),
          `error.helpUrl mentions code`);
  }
}

// ── Run ─────────────────────────────────────────────────────────────

testIoNotFound();
testResampleUnknownSymbol();

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
