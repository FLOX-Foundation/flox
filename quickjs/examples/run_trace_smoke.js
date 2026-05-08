// Round-trip smoke test for the .floxrun bindings under QuickJS.

var path = '/tmp/flox_run_qjs_smoke_' + Date.now();

function check(label, ok) {
  if (!ok) {
    console.log('FAIL: ' + label);
    throw new Error('smoke fail: ' + label);
  }
  console.log('ok: ' + label);
}

var rec = new flox.TraceRecorder({
  path: path,
  strategyId: 'qjs-smoke',
  strategyHash: 'sha256:test',
  runStartedNs: 1700000000000000000,
});
rec.addTapeRef({
  path: 'BTCUSDT.floxlog',
  contentHash: 'sha256:abc',
  firstEventNs: 1700000000000000000,
  lastEventNs: 1700000000500000000,
});
rec.writeSignal({
  runTsNs: 1700000000100000000,
  feedTsNs: 1700000000099000000,
  signalId: 42,
  flags: 1,
  strengthRaw: 75000000,
  name: 'ratio-cross',
  symbolIds: [1, 2],
  payload: '{"src":"ETH","dst":"BTC"}',
});
rec.writeOrderEvent({
  runTsNs: 1700000000200000000,
  feedTsNs: 1700000000099000000,
  orderId: 7,
  parentSignalId: 42,
  priceRaw: 5000000000000,
  qtyRaw: 100000000,
  symbolId: 1,
  eventKind: 1,
  side: 0,
  orderType: 1,
  flags: 1,
  reason: '',
});
rec.writeFill({
  runTsNs: 1700000000300000000,
  feedTsNs: 1700000000250000000,
  orderId: 7,
  fillId: 12345,
  priceRaw: 5000000000000,
  qtyRaw: 100000000,
  feeRaw: 50000,
  symbolId: 1,
  side: 0,
  liquidity: 1,
});
rec.setRunEndedNs(1700000000400000000);
rec.close();

var reader = new flox.TraceReader(path);
check('strategy id', reader.strategyId() === 'qjs-smoke');
check('run ended', reader.runEndedNs() === 1700000000400000000);

var sigs = reader.readAllSignals();
check('signal count', sigs.length === 1);
check('signal id', sigs[0].signalId === 42);
check('signal name', sigs[0].name === 'ratio-cross');
check('signal symbol ids', sigs[0].symbolIds.length === 2 &&
                          sigs[0].symbolIds[0] === 1 && sigs[0].symbolIds[1] === 2);
check('signal payload', sigs[0].payload === '{"src":"ETH","dst":"BTC"}');

var orders = reader.readAllOrderEvents();
check('order count', orders.length === 1);
check('order id', orders[0].orderId === 7);
check('order parent', orders[0].parentSignalId === 42);
check('order kind', orders[0].eventKind === 1);

var fills = reader.readAllFills();
check('fill count', fills.length === 1);
check('fill id', fills[0].fillId === 12345);
check('fill liquidity', fills[0].liquidity === 1);

reader.destroy();
console.log('quickjs run-trace smoke ok');
