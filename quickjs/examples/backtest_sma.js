/**
 * SMA crossover backtest — BTCUSDT 1m data.
 *
 * Demonstrates the QuickJS flox API:
 *   - Engine: load CSV, access OHLCV bars
 *   - SMA (streaming indicator from flox/indicators.js)
 *   - SignalBuilder: accumulate buy/sell signals
 *   - engine.run(): replay signals against bars, return stats
 *
 * Run:
 *   flox_js_runner quickjs/examples/backtest_sma.js
 */

// ── Load data ──────────────────────────────────────────────────────────

var engine = new Engine(10000.0, 0.0004);
engine.loadCsv("examples/data/btcusdt_1m.csv");

var bars = engine._symbols["__default__"];
var n    = bars.length;

console.log("Loaded " + n + " bars  " +
            bars[0].close.toFixed(2) + " → " + bars[n-1].close.toFixed(2));

// ── Signal generation — SMA(10/30) crossover ───────────────────────────

var fast = new SMA(10);
var slow = new SMA(30);
var signals = new SignalBuilder();
var pos = 0;   // -1=short  0=flat  1=long

for (var i = 0; i < n; i++) {
  var b  = bars[i];
  var fv = fast.update(b.close);
  var sv = slow.update(b.close);
  if (!slow.ready) continue;

  // bar.ts is in milliseconds (safe integer range for JS float64)
  var tsMs = b.ts;

  if (fv > sv && pos <= 0) {
    signals.buy(tsMs, pos === 0 ? 0.01 : 0.02);
    pos = 1;
  } else if (fv < sv && pos >= 0) {
    signals.sell(tsMs, pos === 0 ? 0.01 : 0.02);
    pos = -1;
  }
}

console.log(signals.length + " signals generated");

// ── Run backtest ────────────────────────────────────────────────────────

var stats = engine.run(signals);

console.log("\nSMA(10/30) crossover results");
console.log("  Capital  : " + stats.initialCapital.toFixed(2) +
            " → " + stats.finalCapital.toFixed(2));
console.log("  Return   : " + (stats.returnPct >= 0 ? "+" : "") +
            stats.returnPct.toFixed(4) + "%");
console.log("  Trades   : " + stats.totalTrades +
            "  (win rate " + (stats.winRate * 100).toFixed(1) + "%)");
console.log("  Prof.factor: " + stats.profitFactor.toFixed(2));
console.log("  Sharpe   : " + stats.sharpeRatio.toFixed(4));
console.log("  Max DD   : " + stats.maxDrawdownPct.toFixed(4) + "%");
console.log("  Fees     : " + stats.totalFees.toFixed(4));
console.log("  PnL      : " + stats.netPnl.toFixed(4));
