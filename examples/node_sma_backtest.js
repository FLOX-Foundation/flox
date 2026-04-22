/**
 * SMA crossover backtest — BTCUSDT 1m data.
 *
 * Demonstrates the Node.js flox addon API:
 *   - Engine: load CSV, get typed OHLCV arrays
 *   - Batch indicators: rsi, bollinger, macd, atr
 *   - Streaming SMA class for signal generation
 *   - SignalBuilder: accumulate buy/sell signals
 *   - engine.run(): replay signals against bars, return stats object
 *
 * Usage:
 *   node examples/node_sma_backtest.js
 */

const path = require('path');
const flox = require('../node');

const DATA = path.join(__dirname, 'data', 'btcusdt_1m.csv');

// ── Load data ──────────────────────────────────────────────────────────

const engine = new flox.Engine(10_000, 0.0004);
engine.loadCsv(DATA);

const ts    = engine.ts();      // BigInt64Array — nanosecond timestamps
const close = engine.close();   // Float64Array
const high  = engine.high();
const low   = engine.low();
const n     = close.length;

console.log(`Loaded ${n} bars  ${close[0].toFixed(2)} → ${close[n-1].toFixed(2)}`);

// ── Batch indicators ────────────────────────────────────────────────────

const rsi  = flox.rsi(close, 14);
const bb   = flox.bollinger(close, 20, 2.0);
const macd = flox.macd(close, 12, 26, 9);
const atr  = flox.atr(high, low, close, 14);

console.log(
  `RSI=${rsi[n-1].toFixed(1)}  ` +
  `BB=${bb.lower[n-1].toFixed(0)}/${bb.middle[n-1].toFixed(0)}/${bb.upper[n-1].toFixed(0)}  ` +
  `MACD=${macd.line[n-1].toFixed(2)}  ATR=${atr[n-1].toFixed(2)}`
);

// ── Signal generation — SMA(10/30) crossover ───────────────────────────

const fast = new flox.SMA(10);
const slow  = new flox.SMA(30);
const signals = new flox.SignalBuilder();
let pos = 0;   // -1=short  0=flat  1=long

for (let i = 0; i < n; i++) {
  const fv = fast.update(close[i]);
  const sv = slow.update(close[i]);
  if (!slow.ready) continue;

  // Timestamps from engine.ts() are nanoseconds; SignalBuilder expects ms.
  const tsMs = Number(ts[i]) / 1e6;

  if (fv > sv && pos <= 0) {
    signals.buy(tsMs, pos === 0 ? 0.01 : 0.02);
    pos = 1;
  } else if (fv < sv && pos >= 0) {
    signals.sell(tsMs, pos === 0 ? 0.01 : 0.02);
    pos = -1;
  }
}

console.log(`\n${signals.length} signals generated`);

// ── Run backtest ────────────────────────────────────────────────────────

const t0    = process.hrtime.bigint();
const stats = engine.run(signals);
const dt    = Number(process.hrtime.bigint() - t0) / 1e6;

console.log('\nSMA(10/30) crossover results');
console.log(`  Capital  : ${stats.initialCapital.toFixed(2)} → ${stats.finalCapital.toFixed(2)}`);
console.log(`  Return   : ${stats.returnPct > 0 ? '+' : ''}${stats.returnPct.toFixed(4)}%`);
console.log(`  Trades   : ${stats.totalTrades}  (win rate ${(stats.winRate * 100).toFixed(1)}%)`);
console.log(`  Prof.factor: ${stats.profitFactor.toFixed(2)}`);
console.log(`  Sharpe   : ${stats.sharpe.toFixed(4)}`);
console.log(`  Max DD   : ${stats.maxDrawdownPct.toFixed(4)}%`);
console.log(`  Fees     : ${stats.totalFees.toFixed(4)}`);
console.log(`  PnL      : ${stats.netPnl.toFixed(4)}`);
console.log(`  (${dt.toFixed(2)} ms)`);
