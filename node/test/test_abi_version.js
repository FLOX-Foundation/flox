'use strict';
/**
 * node/test/test_abi_version.js — the addon must check the C ABI version
 * when it loads.
 *
 * include/flox/capi/flox_capi.h asks every caller to "compare
 * FLOX_CAPI_ABI_VERSION against flox_capi_abi_version() once at startup
 * and refuse the mismatch": the structs on that boundary carry no
 * reserved tail, so a header/library skew shows up as wrong numbers
 * rather than a failed load. No binding did the comparison.
 *
 * The refusal itself is pinned in C++ (tests/test_capi_abi_check.cpp),
 * which can force a wrong number. What the addon can show is that the
 * check had something to compare: the version it was compiled against
 * and the version the library reports, both exported, and equal.
 *
 * Run from repo root:
 *   cd node && node test/test_abi_version.js
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

console.log('── C ABI version handshake');

check(typeof flox.CAPI_ABI_VERSION === 'number',
  `CAPI_ABI_VERSION is exported as a number (got ${typeof flox.CAPI_ABI_VERSION})`);
check(flox.CAPI_ABI_VERSION > 0,
  `CAPI_ABI_VERSION is positive (${flox.CAPI_ABI_VERSION})`);
check(typeof flox.capiAbiVersion === 'function',
  `capiAbiVersion() is exported (got ${typeof flox.capiAbiVersion})`);

if (typeof flox.capiAbiVersion === 'function') {
  const runtime = flox.capiAbiVersion();
  check(typeof runtime === 'number' && runtime > 0,
    `capiAbiVersion() returns a positive number (${runtime})`);
  check(runtime === flox.CAPI_ABI_VERSION,
    `the addon was built against ABI ${flox.CAPI_ABI_VERSION} and links a ` +
    `library reporting ${runtime}`);
}

// ── Which number is which ─────────────────────────────────────────────
//
// The two exports are equal in any tree built in one go, so their
// equality above cannot tell the compiled-against constant from a second
// read of the runtime function -- and on the install where they do
// differ, require() refuses and neither is observable. The distinction
// lives in the source: CAPI_ABI_VERSION is the macro baked into the
// addon, capiAbiVersion() asks the library it loaded. Reading the
// registration is the only place that is visible from a test.

const fs = require('fs');
const abiSource = fs.readFileSync(path.join(__dirname, '..', 'src', 'abi.h'), 'utf8');

function exportBlock(name) {
  const at = abiSource.indexOf(`exports.Set("${name}"`);
  if (at < 0) return '';
  const end = abiSource.indexOf('exports.Set(', at + 1);
  return abiSource.slice(at, end < 0 ? abiSource.length : end);
}

console.log('\n── which number each export carries');

const compiledBlock = exportBlock('CAPI_ABI_VERSION');
check(compiledBlock !== '', 'node/src/abi.h exports CAPI_ABI_VERSION');
check(compiledBlock.includes('FLOX_CAPI_ABI_VERSION'),
  'CAPI_ABI_VERSION is the compiled-against macro');
check(!compiledBlock.includes('flox_capi_abi_version('),
  'CAPI_ABI_VERSION is not a second read of the runtime function');

const runtimeBlock = exportBlock('capiAbiVersion');
check(runtimeBlock.includes('flox_capi_abi_version('),
  'capiAbiVersion() asks the loaded library');

const checkAt = abiSource.indexOf('checkAbiVersion(FLOX_CAPI_ABI_VERSION');
check(checkAt >= 0, 'the addon compares the two versions when it loads');
check(abiSource.slice(checkAt, checkAt + 300).includes('ThrowAsJavaScriptException'),
  'a mismatch fails the require instead of being reported and ignored');

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
