const path = require('path');
const flox = require('../node');

const DATA = path.join(__dirname, 'data');

const engine = new flox.Engine(10000, 0.0004);
engine.loadCsv(path.join(DATA, 'btcusdt_1m.csv'));
engine.loadCsv(path.join(DATA, 'ethusdt_1m.csv'));

console.log('symbols:', engine.symbols);

const close = engine.close();
const high = engine.high();
const low = engine.low();
const ts = engine.ts();
const n = close.length;

console.log(`${n} bars  ${close[0].toFixed(2)} -> ${close[n-1].toFixed(2)}`);

// -- indicators --
const rsi = flox.rsi(close, 14);
const bb = flox.bollinger(close, 20, 2.0);
const macd = flox.macd(close, 12, 26, 9);
const atr = flox.atr(high, low, close, 14);

console.log(`\nRSI=${rsi[n-1].toFixed(1)}  BB=${bb.lower[n-1].toFixed(0)}/${bb.middle[n-1].toFixed(0)}/${bb.upper[n-1].toFixed(0)}  MACD=${macd.line[n-1].toFixed(1)}  ATR=${atr[n-1].toFixed(1)}`);

// -- SMA crossover signals --
function smaSignals(ts, close, fast = 10, slow = 30, size = 0.01) {
    const sig = new flox.SignalBuilder();
    let fSum = 0, sSum = 0;
    const fBuf = [], sBuf = [];
    let pos = 0;

    for (let i = 0; i < close.length; i++) {
        fBuf.push(close[i]);
        sBuf.push(close[i]);
        fSum += close[i];
        sSum += close[i];
        if (fBuf.length > fast) { fSum -= fBuf.shift(); }
        if (sBuf.length > slow) { sSum -= sBuf.shift(); }

        if (sBuf.length < slow) continue;

        const fv = fSum / fast;
        const sv = sSum / slow;

        if (fv > sv && pos <= 0) {
            sig.buy(ts[i], pos === 0 ? size : size * 2);
            pos = 1;
        } else if (fv < sv && pos >= 0) {
            sig.sell(ts[i], pos === 0 ? size : size * 2);
            pos = -1;
        }
    }

    return sig;
}

const signals = smaSignals(ts, close);
console.log(`${signals.length} signals`);

const t0 = process.hrtime.bigint();
const stats = engine.run(signals);
const dt = Number(process.hrtime.bigint() - t0) / 1e6;

console.log(`\n${stats.initialCapital.toFixed(2)} -> ${stats.finalCapital.toFixed(2)}  ${stats.returnPct > 0 ? '+' : ''}${stats.returnPct.toFixed(4)}%`);
console.log(`trades=${stats.totalTrades} wr=${(stats.winRate*100).toFixed(1)}% pf=${stats.profitFactor.toFixed(2)}`);
console.log(`sharpe=${stats.sharpe.toFixed(4)} dd=${stats.maxDrawdownPct.toFixed(4)}% fees=${stats.totalFees.toFixed(4)}`);
console.log(`(${dt.toFixed(2)}ms)`);

// -- multi-symbol: resample + cross-asset --
engine.resample("BTCUSDT", "BTCUSDT_5m", "5m");
engine.resample("ETHUSDT", "ETHUSDT_5m", "5m");

const btc5m = engine.close("BTCUSDT_5m");
const eth5m = engine.close("ETHUSDT_5m");
console.log(`\n5m bars: BTC=${engine.barCount("BTCUSDT_5m")} ETH=${engine.barCount("ETHUSDT_5m")}`);
console.log(`5m last: BTC=${btc5m[btc5m.length-1].toFixed(2)} ETH=${eth5m[eth5m.length-1].toFixed(2)}`);

// -- ETH RSI strategy --
const ethClose = engine.close("ETHUSDT");
const ethTs = engine.ts("ETHUSDT");
const ethRsi = flox.rsi(ethClose, 14);

const ethSig = new flox.SignalBuilder();
let ethPos = 0;
for (let i = 14; i < ethClose.length; i++) {
    if (ethRsi[i] < 30 && ethPos <= 0) {
        ethSig.buy(ethTs[i], 0.1, "ETHUSDT");
        ethPos = 1;
    } else if (ethRsi[i] > 70 && ethPos >= 0) {
        ethSig.sell(ethTs[i], 0.1, "ETHUSDT");
        ethPos = -1;
    }
}

const ethStats = engine.run(ethSig);
console.log(`\nETH RSI strategy: ${ethStats.totalTrades} trades, pnl=$${ethStats.netPnl.toFixed(2)}, sharpe=${ethStats.sharpe.toFixed(4)}`);
