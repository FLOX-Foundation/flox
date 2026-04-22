// SMA crossover backtest using Engine + SignalBuilder.
//
// Run via the flox-js CLI:
//   flox-js quickjs/examples/backtest_sma.js

var FAST = 10;
var SLOW = 30;
var QTY  = 0.01;

var engine  = new Engine(10000.0, 0.0004);
var signals = new SignalBuilder();

engine.loadCsv("examples/data/btcusdt_1m.csv");

var bars = engine._symbols["__default__"];
var fastSma = new SMA(FAST);
var slowSma = new SMA(SLOW);

var inLong   = false;
var prevAbove = false;

for (var i = 0; i < bars.length; i++) {
    var b = bars[i];
    var tsMs = Math.floor(b.ts / 1e6);
    var fast = fastSma.update(b.close);
    var slow = slowSma.update(b.close);
    if (!slowSma.ready) {
        prevAbove = fast > slow;
        continue;
    }
    var above = fast > slow;
    if (above && !prevAbove && !inLong) {
        signals.buy(tsMs, QTY);
        inLong = true;
    } else if (!above && prevAbove && inLong) {
        signals.sell(tsMs, QTY);
        inLong = false;
    }
    prevAbove = above;
}

var stats = engine.run(signals);
print("SMA(" + FAST + "/" + SLOW + ") backtest");
print("  Bars    : " + engine.barCount);
print("  Signals : " + signals.length);
print("  Trades  : " + stats.totalTrades);
print("  Win rate: " + (stats.winRate * 100).toFixed(1) + "%");
print("  Net PnL : " + stats.netPnl.toFixed(2));
print("  Return  : " + (stats.returnPct * 100).toFixed(2) + "%");
print("  Max DD  : " + (stats.maxDrawdownPct * 100).toFixed(1) + "%");
print("  Sharpe  : " + stats.sharpe.toFixed(2));
