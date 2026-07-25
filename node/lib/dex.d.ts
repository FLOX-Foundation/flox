// Type declarations for the `@flox-foundation/flox/lib/dex` subpath import.
//
// Single source of truth: the `dex` namespace in ../index.d.ts, which is
// derived from lib/dex.js. This file only re-exports it, so the subpath
// import and the `.dex` property of the main module cannot drift apart.
//
// An earlier version duplicated ~140 lines of declarations here on the
// theory that the d.ts-export parity gate rejects names without a NAPI
// export. It does not: scripts/check_dts_exports.py skips
// `export namespace`. The duplicate had also drifted, typing every amount
// as `string | number | bigint | Amount` when Amount.parse throws on a
// bare number for swap and reserve inputs.

import { dex } from '../index';

export = dex;
