// Produce the same deterministic .floxrun directory through the NAPI
// binding as scripts/floxrun_byte_identical_fixture.py. The
// cross-binding gate hashes the segment files and asserts they match.

const flox = require('..');

function writeFixture(outDir) {
  const rec = new flox.TraceRecorder({
    path: outDir,
    strategyId: 'byte-identical-gate',
    strategyHash: 'sha256:fixture',
    runStartedNs: 1700000000000000000,
  });
  rec.writeSignal({
    runTsNs: 1700000000100000000,
    feedTsNs: 1700000000099000000,
    signalId: 42,
    flags: 1, // SIGNAL_FLAG_ENTER
    strengthRaw: 75000000,
    name: 'entry',
    symbolIds: [1, 2],
    payload: Buffer.alloc(0),
  });
  rec.writeSignal({
    runTsNs: 1700000000200000000,
    feedTsNs: 1700000000199000000,
    signalId: 43,
    flags: 2, // SIGNAL_FLAG_EXIT
    strengthRaw: 0,
    name: 'exit',
    symbolIds: [1],
    payload: Buffer.alloc(0),
  });
  rec.writeOrderEvent({
    runTsNs: 1700000000150000000,
    feedTsNs: 1700000000149000000,
    orderId: 7,
    parentSignalId: 42,
    priceRaw: 5000000000000,
    qtyRaw: 100000000,
    symbolId: 1,
    eventKind: 1, // Submit (matches OrderEventKind in run_format_v1.h)
    side: 0,
    orderType: 1,
    flags: 0,
    reason: '',
  });
  rec.writeFill({
    runTsNs: 1700000000175000000,
    feedTsNs: 1700000000174000000,
    orderId: 7,
    fillId: 12345,
    priceRaw: 5000000000000,
    qtyRaw: 100000000,
    feeRaw: 50000,
    symbolId: 1,
    side: 0,
    liquidity: 1, // Maker
  });
  rec.setRunEndedNs(1700000000400000000);
  rec.close();
}

const outDir = process.argv[2];
if (!outDir) {
  console.error(`usage: node ${process.argv[1]} <out-dir>`);
  process.exit(2);
}
writeFixture(outDir);
