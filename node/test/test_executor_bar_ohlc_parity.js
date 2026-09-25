// The bar path of the Node addon against the bar path of the engine.
//
// BacktestRunner.runBars walks every bar open -> low -> high -> close, holds
// whatever the bar callback submits, and releases it at the next bar's open. A
// caller driving SimulatedExecutor by hand has none of that: the addon exposes
// onBar(symbol, close) and nothing else, so it moves the market straight to
// the close, never sees an intrabar extreme, and matches a callback order at a
// price that exists only because the bar has already happened.
//
// Needed on SimulatedExecutor:
//
//   onBarOhlc(symbol, open, high, low, close)
//   beginBarCallbackWindow()
//   endBarCallbackWindow()
//   reset()
//
// and closeReason on the bars aggregate*Bars returns.
//
// Sister cases: tests/test_capi_executor_bar_ohlc.cpp and
// python/tests/test_executor_bar_ohlc_parity.py, on the same tape.

const flox = require('..');

const failures = [];
function check(label, ok, detail) {
  if (ok) {
    console.log('ok: ' + label);
    return;
  }
  failures.push(label + (detail ? ' -- ' + detail : ''));
  console.error('FAIL: ' + label + (detail ? ' -- ' + detail : ''));
}

const MINUTE_NS = 60000000000n;
const SYMBOL = 1;

// Bar 0 closes at 105 and bar 1 opens at 106 -- a price that appears nowhere
// in bar 0. Bar 1 runs its high to 109.5, above the resting sell at 109 and
// above its own close of 107, so a fill there can only come from the intrabar
// extreme.
const TAPE = [
  [100.0, 110.0, 90.0, 105.0],
  [106.0, 109.5, 104.0, 107.0],
  [103.0, 104.0, 102.0, 103.5],
];

// A buy at the next bar's open (106) closed out by the resting sell at 109.
// A limit order can only trade at the price it posted, so the realised pnl
// names the price the held buy got: 3.0 means 106, and 2.0 means the close of
// the bar the callback was shown to (107).
const EXPECTED_NET_PNL = 3.0;

const REQUIRED = [
  ['onBarOhlc', 'onBarOhlc(symbol, open, high, low, close)'],
  ['beginBarCallbackWindow', 'beginBarCallbackWindow()'],
  ['endBarCallbackWindow', 'endBarCallbackWindow()'],
  ['reset', 'reset()'],
];

function missingApi(exec) {
  return REQUIRED.filter(([name]) => typeof exec[name] !== 'function').map(
    ([, signature]) => signature,
  );
}

function barArrays() {
  const start = new BigInt64Array(TAPE.map((_, i) => BigInt(i) * MINUTE_NS));
  const end = new BigInt64Array(TAPE.map((_, i) => BigInt(i + 1) * MINUTE_NS));
  const col = (k) => Float64Array.from(TAPE.map((row) => row[k]));
  const volume = Float64Array.from(TAPE.map(() => 1000.0));
  return [start, end, col(0), col(1), col(2), col(3), volume];
}

// The reference: what the runner does with the same orders on the same tape.
function runnerRun() {
  const registry = new flox.SymbolRegistry();
  const symbol = registry.addSymbol('backtest', 'BTCUSDT', 0.01);
  const runner = new flox.BacktestRunner(registry, 0.0, 100000.0);

  const fills = [];
  let bars = 0;
  runner.setStrategy({
    symbols: [symbol],
    onBar(ctx, bar, emit) {
      if (bars === 0) {
        emit.marketBuy(1.0);
        emit.limitSell(109.0, 1.0);
      }
      bars++;
    },
    onFill(ctx, ev) {
      fills.push([ev.side, ev.fillPrice, ev.fillQty]);
    },
  });

  const stats = runner.runBars(...barArrays(), 'BTCUSDT');
  return { fills, stats };
}

// The hand-drive: exactly what runBars does per bar, spelled through the addon.
function driveByHand(exec) {
  let sent = false;
  TAPE.forEach(([open, high, low, close], i) => {
    exec.advanceClock(BigInt(i + 1) * MINUTE_NS);
    exec.onBarOhlc(SYMBOL, open, high, low, close);

    exec.beginBarCallbackWindow();
    if (!sent) {
      sent = true;
      exec.submitOrder(1, 'buy', 0.0, 1.0, 'market', SYMBOL);
      exec.submitOrder(2, 'sell', 109.0, 1.0, 'limit', SYMBOL);
    }
    exec.endBarCallbackWindow();
  });
}

function netPnlOf(exec) {
  const result = new flox.BacktestResult(100000.0, 0.0);
  result.ingestExecutor(exec);
  return result.stats().netPnl;
}

// ── green control: the runner path is the reference ────────────────────────

const reference = runnerRun();
check(
  'the runner fills the callback order at the next open and the resting sell at its price',
  JSON.stringify(reference.fills) === JSON.stringify([['buy', 106, 1], ['sell', 109, 1]]),
  JSON.stringify(reference.fills),
);
check(
  'the reference run realises ' + EXPECTED_NET_PNL,
  reference.stats.netPnl === EXPECTED_NET_PNL,
  'netPnl=' + reference.stats.netPnl,
);

// ── the surface ────────────────────────────────────────────────────────────

const probe = new flox.SimulatedExecutor();
const missing = missingApi(probe);
check('SimulatedExecutor has onBar, the close-only form', typeof probe.onBar === 'function');
check(
  'SimulatedExecutor exposes the full bar path',
  missing.length === 0,
  'missing ' + missing.join(', ') + '; a caller driving the executor by hand ' +
    'gets neither the bar open nor the intrabar extremes',
);

// ── behaviour, once the surface is there ───────────────────────────────────

if (missing.length === 0) {
  {
    const exec = new flox.SimulatedExecutor();
    driveByHand(exec);
    check('the hand-driven run fills twice', exec.fillCount === 2, 'fillCount=' + exec.fillCount);
    check(
      'the hand-driven run realises what the runner realises',
      netPnlOf(exec) === reference.stats.netPnl,
      'byHand=' + netPnlOf(exec) + ' runner=' + reference.stats.netPnl,
    );
  }

  {
    // The mutation this is shaped against: onBarOhlc forwarding the close
    // where the open belongs. The held buy would then pay 107, not 106.
    const exec = new flox.SimulatedExecutor();
    driveByHand(exec);
    const pnl = netPnlOf(exec);
    check(
      'a held order fills at the next bar open, not at a close',
      pnl === EXPECTED_NET_PNL,
      'netPnl=' + pnl + '; 2 would mean the held buy paid a close of 107',
    );
  }

  {
    // Without the window the order is not held at all: it matches inside the
    // bar it was submitted from, at the close.
    const exec = new flox.SimulatedExecutor();
    const [open, high, low, close] = TAPE[0];
    exec.advanceClock(MINUTE_NS);
    exec.onBarOhlc(SYMBOL, open, high, low, close);
    exec.submitOrder(1, 'buy', 0.0, 1.0, 'market', SYMBOL);
    check(
      'an order submitted outside the window is not held',
      exec.fillCount === 1,
      'fillCount=' + exec.fillCount,
    );
  }

  {
    // reset() is what makes a second hand-driven run report that run.
    const exec = new flox.SimulatedExecutor();
    driveByHand(exec);
    const first = netPnlOf(exec);
    exec.reset();
    check('reset clears the previous run fills', exec.fillCount === 0, 'fillCount=' + exec.fillCount);
    driveByHand(exec);
    check(
      'the second run repeats the first',
      exec.fillCount === 2 && netPnlOf(exec) === first,
      'fillCount=' + exec.fillCount + ' netPnl=' + netPnlOf(exec) + ' first=' + first,
    );
  }
}

// ── which intrabar extreme the walk reaches first ──────────────────────────
//
// One resting order cannot say which price arrived as the high and which as
// the low: a bar whose range straddles it touches it either way round. Two
// resting orders on opposite sides of the same bar do say it, as long as only
// one of them can survive -- a bracket, where the first child to fill cancels
// the other. The engine walks low before high (the pessimistic order for a
// long: the protective stop is tested before the target), so the stop has to
// be the child that fills.
//
// The runner cannot be handed a bracket from Node -- BacktestRunner owns its
// executor and exposes no way to reach it -- so the runner comparison here
// covers the part it can express, the entry: a plain market buy from the same
// bar-0 callback on the same tape. tests/test_capi_executor_bar_ohlc.cpp
// holds the full runner comparison.

const BRACKET_ID = 7;
const TAKE_PROFIT_PRICE = 106.0;
const STOP_TRIGGER_PRICE = 94.0;

// Flat bar to arm the bracket, then a bar straddling both of its children.
const BRACKET_TAPE = [
  [100.0, 100.0, 100.0, 100.0],
  [100.0, 108.0, 92.0, 100.0],
];

function bracketBarArrays() {
  const start = new BigInt64Array(BRACKET_TAPE.map((_, i) => BigInt(i) * MINUTE_NS));
  const end = new BigInt64Array(BRACKET_TAPE.map((_, i) => BigInt(i + 1) * MINUTE_NS));
  const col = (k) => Float64Array.from(BRACKET_TAPE.map((row) => row[k]));
  const volume = Float64Array.from(BRACKET_TAPE.map(() => 1000.0));
  return [start, end, col(0), col(1), col(2), col(3), volume];
}

function runnerEntryFill() {
  const registry = new flox.SymbolRegistry();
  const symbol = registry.addSymbol('backtest', 'BTCUSDT', 0.01);
  const runner = new flox.BacktestRunner(registry, 0.0, 100000.0);
  const fills = [];
  let bars = 0;
  runner.setStrategy({
    symbols: [symbol],
    onBar(ctx, bar, emit) {
      if (bars === 0) emit.marketBuy(1.0);
      bars++;
    },
    onFill(ctx, ev) {
      fills.push([ev.side, ev.fillPrice, ev.fillQty]);
    },
  });
  runner.runBars(...bracketBarArrays(), 'BTCUSDT');
  return fills;
}

function driveBracket(exec) {
  let armed = false;
  BRACKET_TAPE.forEach(([open, high, low, close], i) => {
    exec.advanceClock(BigInt(i + 1) * MINUTE_NS);
    exec.onBarOhlc(SYMBOL, open, high, low, close);

    exec.beginBarCallbackWindow();
    if (!armed) {
      armed = true;
      exec.submitBracket({
        bracketId: BRACKET_ID,
        symbol: SYMBOL,
        entrySide: 'buy',
        entryType: 'market',
        entryPrice: 0.0,
        quantity: 1.0,
        tpSide: 'sell',
        tpType: 'limit',
        tpPrice: TAKE_PROFIT_PRICE,
        stopSide: 'sell',
        stopType: 'stop_market',
        stopTriggerPrice: STOP_TRIGGER_PRICE,
      });
    }
    exec.endBarCallbackWindow();
  });
}

if (missing.length === 0) {
  const entry = runnerEntryFill();
  check(
    'the runner fills the entry at the next bar open',
    JSON.stringify(entry) === JSON.stringify([['buy', 100, 1]]),
    JSON.stringify(entry),
  );

  const exec = new flox.SimulatedExecutor();
  driveBracket(exec);

  check(
    'the walk reaches the low before the high',
    exec.bracketState(BRACKET_ID) === 'stop_filled',
    'bracketState=' + exec.bracketState(BRACKET_ID) +
      '; the take-profit at ' + TAKE_PROFIT_PRICE + ' filled instead of the stop',
  );
  check('the bracket produced the entry and one child', exec.fillCount === 2,
        'fillCount=' + exec.fillCount);
  // Entry at 100 closed by the stop at the bar low of 92: a loss of 8. Had the
  // take-profit at 106 been the survivor it would read +6.
  const pnl = netPnlOf(exec);
  check(
    'the surviving child is the stop, not the take-profit',
    pnl === -8.0,
    'netPnl=' + pnl + '; +6 would mean the take-profit filled',
  );
}

// ── close_reason on aggregated bars ────────────────────────────────────────

{
  const ts = Float64Array.from([0, 30e9, 61e9, 91e9, 121e9]);
  const px = Float64Array.from([100.0, 101.0, 102.0, 103.0, 104.0]);
  const qty = Float64Array.from([1, 1, 1, 1, 1]);
  const isBuy = Uint8Array.from([1, 1, 1, 1, 1]);
  const bars = flox.aggregateTimeBars(ts, px, qty, isBuy, 60.0);

  check('the tape aggregates into at least one bar', bars.length >= 1);
  const hasField = bars.length >= 1 && Object.hasOwn(bars[0], 'closeReason');
  check(
    'aggregated bars carry closeReason',
    hasField,
    bars.length >= 1
      ? 'fields are ' + Object.keys(bars[0]).join(', ') + '; BarData already ' +
        'has closeReason on the live path'
      : 'no bars',
  );
  if (hasField) {
    // 0 = Threshold: every bar the batch path returns was closed by its own
    // threshold, and the field has to say so rather than report a leftover.
    check(
      'every aggregated bar reports the threshold close',
      bars.every((b) => b.closeReason === 0),
      JSON.stringify(bars.map((b) => b.closeReason)),
    );
  }
}

if (failures.length > 0) {
  console.error('\n' + failures.length + ' check(s) failed');
  process.exit(1);
}
console.log('\nall checks passed');
