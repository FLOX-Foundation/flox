/**
 * JavaScript Backtest Demo -- exercises the full JS API.
 *
 * Generates synthetic data, runs indicators, executes orders through
 * SimulatedExecutor, tracks positions, and prints results.
 *
 * Usage:
 *     ./build/src/quickjs/flox_js_runner examples/js_backtest_demo.js
 */

// -- Generate synthetic price data --
var N = 2000;
var prices = new Array(N);
var highs = new Array(N);
var lows = new Array(N);
var volumes = new Array(N);

prices[0] = 50000;
for (var i = 1; i < N; i++) {
    // Simple random walk with drift
    var ret = (Math.random() - 0.498) * 0.003;
    prices[i] = prices[i - 1] * (1 + ret);
    highs[i] = prices[i] * (1 + Math.random() * 0.002);
    lows[i] = prices[i] * (1 - Math.random() * 0.002);
    volumes[i] = Math.random() * 100 + 10;
}
highs[0] = prices[0] * 1.001;
lows[0] = prices[0] * 0.999;
volumes[0] = 50;

console.log("Generated " + N + " bars");
console.log("  Start: " + prices[0].toFixed(2) + ", End: " + prices[N-1].toFixed(2));

// -- Streaming indicators --
var sma10 = new SMA(10);
var sma30 = new SMA(30);
var rsi = new RSI(14);
var macd = new MACD(12, 26, 9);
var bb = new Bollinger(20, 2.0);
var atr = new ATR(14);
var stoch = new Stochastic(14, 3);
var obv = new OBV();

for (var i = 0; i < N; i++) {
    sma10.update(prices[i]);
    sma30.update(prices[i]);
    rsi.update(prices[i]);
    macd.update(prices[i]);
    bb.update(prices[i]);
    atr.update(highs[i], lows[i], prices[i]);
    stoch.update(highs[i], lows[i], prices[i]);
    obv.update(prices[i], volumes[i]);
}

console.log("\n=== Final Indicator Values ===");
console.log("  SMA(10):     " + sma10.value.toFixed(2));
console.log("  SMA(30):     " + sma30.value.toFixed(2));
console.log("  RSI(14):     " + rsi.value.toFixed(2));
console.log("  MACD line:   " + macd.line.toFixed(4));
console.log("  MACD signal: " + macd.signal.toFixed(4));
console.log("  MACD hist:   " + macd.histogram.toFixed(4));
console.log("  BB upper:    " + bb.upper.toFixed(2));
console.log("  BB middle:   " + bb.middle.toFixed(2));
console.log("  BB lower:    " + bb.lower.toFixed(2));
console.log("  ATR(14):     " + atr.value.toFixed(2));
console.log("  Stoch %K:    " + stoch.k.toFixed(2));
console.log("  Stoch %D:    " + stoch.d.toFixed(2));
console.log("  OBV:         " + obv.value.toFixed(0));

// -- Batch indicator computation --
var adxResult = ADX.compute(highs, lows, prices, 14);
console.log("  ADX(14):     " + adxResult.adx[N-1].toFixed(2));
console.log("  +DI:         " + adxResult.plusDi[N-1].toFixed(2));
console.log("  -DI:         " + adxResult.minusDi[N-1].toFixed(2));

// -- Order Book --
var book = new OrderBook(0.01);
book.applySnapshot(
    [50000, 49999, 49998, 49997, 49996],
    [1.5, 2.0, 3.0, 1.0, 0.5],
    [50001, 50002, 50003, 50004, 50005],
    [0.5, 1.0, 2.0, 1.5, 0.3]
);

console.log("\n=== Order Book ===");
console.log("  Best bid: " + book.bestBid() + " | Best ask: " + book.bestAsk());
console.log("  Mid: " + book.mid().toFixed(2) + " | Spread: " + book.spread().toFixed(2));

var bids = book.getBids(3);
var asks = book.getAsks(3);
console.log("  Top 3 bids: " + JSON.stringify(bids));
console.log("  Top 3 asks: " + JSON.stringify(asks));

// -- Simulated Execution: SMA crossover --
console.log("\n=== Simulated SMA Crossover ===");
var executor = new SimulatedExecutor();
var tracker = new PositionTracker();
var sym = 1;
var orderId = 1;
var position = 0;
var tradeCount = 0;

sma10 = new SMA(10);
sma30 = new SMA(30);
var prevFastAbove = false;

for (var i = 0; i < N; i++) {
    var ts = 1704067200000000000 + i * 60000000000;
    executor.advanceClock(ts);
    executor.onBar(sym, prices[i]);

    var fast = sma10.update(prices[i]);
    var slow = sma30.update(prices[i]);

    if (!sma30.ready) {
        prevFastAbove = fast > slow;
        continue;
    }

    var fastAbove = fast > slow;

    if (fastAbove && !prevFastAbove && position <= 0) {
        if (position < 0) {
            executor.submitOrder(orderId++, "buy", 0, 1.0, 0, sym);
            tradeCount++;
        }
        executor.submitOrder(orderId++, "buy", 0, 1.0, 0, sym);
        position = 1;
        tradeCount++;
    } else if (!fastAbove && prevFastAbove && position >= 0) {
        if (position > 0) {
            executor.submitOrder(orderId++, "sell", 0, 1.0, 0, sym);
            tradeCount++;
        }
        executor.submitOrder(orderId++, "sell", 0, 1.0, 0, sym);
        position = -1;
        tradeCount++;
    }

    prevFastAbove = fastAbove;
}

// Record fills in position tracker
var fillCount = executor.fillCount;
console.log("  Trades executed: " + tradeCount);
console.log("  Fills: " + fillCount);

// -- Volume Profile --
var vp = new VolumeProfile(1.0);
for (var i = 0; i < N; i++) {
    vp.addTrade(prices[i], volumes[i], Math.random() > 0.5);
}
console.log("\n=== Volume Profile ===");
console.log("  POC: " + vp.poc().toFixed(2));
console.log("  Value Area High: " + vp.valueAreaHigh().toFixed(2));
console.log("  Value Area Low:  " + vp.valueAreaLow().toFixed(2));

// -- Statistics --
var returns = [];
for (var i = 1; i < N; i++) {
    returns.push((prices[i] - prices[i-1]) / prices[i-1]);
}
console.log("\n=== Statistics ===");
console.log("  Win rate:        " + (flox.winRate(returns) * 100).toFixed(1) + "%");
console.log("  Profit factor:   " + flox.profitFactor(returns).toFixed(4));

var ci = flox.bootstrapCI(returns, 0.95, 5000);
console.log("  95% CI: [" + (ci.lower * 100).toFixed(4) + "%, " + (ci.upper * 100).toFixed(4) + "%]");
console.log("  Median return:   " + (ci.median * 100).toFixed(4) + "%");

console.log("\nDone.");

// Runner expects a registered strategy -- register a no-op to avoid error
class Noop extends Strategy {
    constructor() { super({ exchange: "X", symbols: ["X"] }); }
}
flox.register(new Noop());
