'use strict';
/**
 * node/test/test_delta_book.js — DeltaBookEncoder / DeltaBookReplayer.
 */

const path = require('path');
const flox = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const lvl = (price, qty) => ({ priceRaw: price, qtyRaw: qty });

console.log('=== Encoder basics ===');
{
  const enc = new flox.DeltaBookEncoder({ anchorEvery: 10 });
  const first = enc.encode(1, [lvl(10000, 10), lvl(9999, 5)], [lvl(10001, 8)]);
  check(first.isDelta === false, 'first event is a snapshot');
  check(first.bids.length === 2 && first.asks.length === 1,
        'snapshot carries the full level set');
}

console.log('=== Anchor cadence ===');
{
  const enc = new flox.DeltaBookEncoder({ anchorEvery: 3 });
  const bids = [lvl(10000, 10)];
  const asks = [lvl(10001, 5)];
  const kinds = [];
  for (let i = 0; i < 5; i++) {
    kinds.push(enc.encode(1, bids, asks).isDelta);
  }
  check(JSON.stringify(kinds) === JSON.stringify([false, true, true, false, true]),
        'cadence: snapshot, delta, delta, snapshot, delta');
}

console.log('=== Delta records changes + removals ===');
{
  const enc = new flox.DeltaBookEncoder({ anchorEvery: 100 });
  enc.encode(1, [lvl(10000, 10), lvl(9999, 5)], [lvl(10001, 8)]);
  const next = enc.encode(1, [lvl(10000, 12)], [lvl(10001, 8), lvl(10002, 3)]);
  check(next.isDelta === true, 'second event is a delta');
  const bidDiff = Object.fromEntries(next.bids.map(l => [l.priceRaw, l.qtyRaw]));
  check(bidDiff[9999] === 0 && bidDiff[10000] === 12,
        'bid delta records removal of 9999 and change of 10000');
  const askDiff = Object.fromEntries(next.asks.map(l => [l.priceRaw, l.qtyRaw]));
  check(askDiff[10002] === 3 && askDiff[10001] === undefined,
        'ask delta records new 10002 only');
}

console.log('=== Replayer reconstructs full snapshot ===');
{
  const enc = new flox.DeltaBookEncoder({ anchorEvery: 100 });
  const rep = new flox.DeltaBookReplayer();
  const sequences = [
    [[lvl(10000, 10), lvl(9999, 5)], [lvl(10001, 8)]],
    [[lvl(10000, 12)], [lvl(10001, 8), lvl(10002, 3)]],
    [[lvl(10000, 12), lvl(9998, 1)], [lvl(10001, 7)]],
  ];
  let allMatch = true;
  for (const [bids, asks] of sequences) {
    const ev = enc.encode(1, bids, asks);
    const kind = ev.isDelta ? 1 : 0;
    const replayed = rep.apply(kind, 1, ev.bids, ev.asks);
    const norm = side => side.map(l => `${l.priceRaw}:${l.qtyRaw}`).sort().join(',');
    if (norm(replayed.bids) !== norm(bids) || norm(replayed.asks) !== norm(asks)) {
      allMatch = false;
      break;
    }
  }
  check(allMatch, 'replayer reconstruction matches the original snapshots');
}

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
