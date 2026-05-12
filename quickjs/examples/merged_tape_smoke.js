// Smoke test for MergedTapeReader exposed on the QuickJS host.
//
// Records two tapes via BinaryLogRecorderHook with distinct (exchange, name)
// pairs, then exercises:
//   - MergedTapeReader.symbolTable() shows two entries (one per tape)
//   - perTapeStats() reports two tapes with the right trade counts
//   - timeRange() spans both tapes
//   - readTrades() returns the merged, time-sorted trade stream

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

var tmpRoot = '/tmp/flox-merged-tape-qjs-smoke-' + Date.now() + '-' + Math.floor(Math.random() * 1000000);
var dirA = tmpRoot + '/a';
var dirB = tmpRoot + '/b';

// Write a tape using BinaryLogRecorderHook + the recorder-handle drivers.
// The drivers push lifecycle + trade events through the same callback path
// the engine uses, so metadata.json is produced and the tape is mergeable.
function writeTape(dir, exchangeName, symId, symName, trades) {
    // exchangeName feeds RecordingMetadata.exchange — MergedTapeReader keys
    // tapes by (exchange, name), so distinct exchanges or distinct names
    // both yield distinct global ids.
    var h = new BinaryLogRecorderHook(dir, 0, 0, 'none', exchangeName, 'perpetual');
    h.addSymbol(symId, symName, '', '', 2, 6);
    var rec = h._asRecorderHandle();
    __flox_recorder_on_start(rec);
    for (var i = 0; i < trades.length; i++) {
        var t = trades[i];
        __flox_recorder_on_trade(rec, symId, t.price, t.qty,
                                 t.side === 'buy', t.tsNs);
    }
    __flox_recorder_on_stop(rec);
    h.destroy();
}

var tradesA = [
    { tsNs: 1000, price: 100.0, qty: 1.0, side: 'buy'  },
    { tsNs: 3000, price: 102.0, qty: 2.0, side: 'sell' },
];
var tradesB = [
    { tsNs: 2000, price: 200.0, qty: 0.5, side: 'sell' },
    { tsNs: 4000, price: 201.0, qty: 1.5, side: 'buy'  },
];

// Use distinct exchange names; symbol names happen to differ too.
// Both factors keep the merged global ids separate.
writeTape(dirA, 'venueA', 1, 'AAA', tradesA);
writeTape(dirB, 'venueB', 2, 'BBB', tradesB);

var mtr = new MergedTapeReader([dirA, dirB]);

// symbolTable: two distinct symbols (AAA, BBB).
var syms = mtr.symbolTable();
check(syms.length === 2, 'symbolTable: two entries (got ' + syms.length + ')');
var names = syms.map(function(s) { return s.name; }).sort();
check(names[0] === 'AAA' && names[1] === 'BBB',
      'symbolTable: names AAA + BBB (got ' + JSON.stringify(names) + ')');

// perTapeStats: two tapes in input order, with time bounds.
// Note: trades/books counters are populated as a side effect of
// readTrades / readBooks, so we re-fetch perTapeStats after the read.
var perTape = mtr.perTapeStats();
check(perTape.length === 2, 'perTapeStats: two tapes (got ' + perTape.length + ')');
check(perTape[0].firstEventNs === 1000n && perTape[0].lastEventNs === 3000n,
      'perTapeStats[0] time bounds 1000..3000');
check(perTape[1].firstEventNs === 2000n && perTape[1].lastEventNs === 4000n,
      'perTapeStats[1] time bounds 2000..4000');

// timeRange spans both tapes (1000 .. 4000).
var tr = mtr.timeRange();
check(tr.minFirstNs === 1000n,
      'timeRange.minFirstNs === 1000 (got ' + tr.minFirstNs + ')');
check(tr.maxLastNs === 4000n,
      'timeRange.maxLastNs === 4000 (got ' + tr.maxLastNs + ')');

// countTrades / readTrades: merged, time-sorted.
check(mtr.countTrades() === 4n,
      'countTrades === 4 (got ' + mtr.countTrades() + ')');
var merged = mtr.readTrades();
check(merged.length === 4, 'readTrades length 4 (got ' + merged.length + ')');

// After readTrades() the per-tape counter reflects the cached read.
var perTape2 = mtr.perTapeStats();
check(perTape2[0].trades === 2n && perTape2[1].trades === 2n,
      'perTapeStats trades == 2 per tape after readTrades');

var sortedAsc = true;
for (var i = 1; i < merged.length; i++) {
    if (merged[i].exchangeTsNs < merged[i - 1].exchangeTsNs) { sortedAsc = false; break; }
}
check(sortedAsc, 'readTrades: output is time-sorted ascending');

// Sequence of timestamps should be 1000, 2000, 3000, 4000 — interleaved.
var ts = merged.map(function(t) { return t.exchangeTsNs; });
check(ts[0] === 1000 && ts[1] === 2000 && ts[2] === 3000 && ts[3] === 4000,
      'readTrades: timestamps interleaved 1000/2000/3000/4000 (got ' + JSON.stringify(ts) + ')');

mtr.destroy();

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('merged tape smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
