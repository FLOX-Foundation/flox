// W14-T012 — verify Runner.attachTraceRecorder() compiles + accepts both
// a TraceRecorder instance and null. End-to-end signal capture is
// exercised via the Python pytest suite where the engine drives signals
// through the runner.

const flox = require('..');
const fs = require('fs');
const os = require('os');
const path = require('path');

function check(label, ok) {
  if (!ok) { console.error('FAIL: ' + label); process.exit(1); }
  console.log('ok: ' + label);
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'flox-trace-attach-'));
const runPath = path.join(tmp, 'run.floxrun');

const rec = new flox.TraceRecorder({
  path: runPath,
  strategyId: 'node-attach-test',
  strategyHash: 'sha256:test',
  runStartedNs: Date.now() * 1000000,
});

const reg = new flox.SymbolRegistry();
const runner = new flox.Runner(reg, () => {});

runner.attachTraceRecorder(rec);
check('attach a recorder', true);

runner.attachTraceRecorder(null);
check('detach with null', true);

runner.attachTraceRecorder(rec);
check('attach again', true);

runner.setTraceFeedTsNs(123456789);
check('setTraceFeedTsNs accepts a number', true);

rec.close();
fs.rmSync(tmp, { recursive: true, force: true });

console.log('node trace_attach test ok');
