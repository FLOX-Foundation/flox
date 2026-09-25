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

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
