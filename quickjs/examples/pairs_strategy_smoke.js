// Pairs trading strategy -- QuickJS mirror of codon/examples/pairs_strategy.codon.
//
// Same logic as the Codon version: track the spread between two legs'
// mid prices, enter when the z-score of the spread over a lookback window
// clears an entry threshold, exit when it reverts inside an exit
// threshold. Buys/sells are plain market orders on both legs, sized by
// a fixed hedge ratio against leg1's order size.
//
// Unlike sma_crossover.js (a demo strategy that flox_js_runner only
// loads and starts/stops -- no data is fed to it in CI), this file also
// drives the strategy against a synthetic input at module scope and
// checks the emitted signals, so it runs as a real smoke test.
//
// The driver below is the same one added to codon/examples/pairs_strategy.codon
// (see that file and the T058 PR that added it): leg2's book is set once and
// never moves again, so every later book update -- fired for leg1 alone --
// reads a spread controlled directly by leg1's mid. A 10-tick warmup around
// spread 5, then a jump to 22, then one tick back down: entry_z=1.5 clears
// at the jump, and exit_z=0.5 already clears on the very next tick (the
// window's mean has not caught up yet), for one entry and one exit -- 4
// emitted orders total. Verified independently in Python (see the PR
// description) and cross-checked against this exact JS run.

class PairsStrategy extends Strategy {
    constructor(cfg) {
        super({ exchange: "Binance", symbols: ["AAAUSDT", "BBBUSDT"] });
        this.cfg = cfg;
        this.spreadHistory = [];
        this.isLongSpread = false;
        this.inPosition = false;
        this.tradeCount = 0;
    }

    onBookUpdate(ctx, book) {
        var mid1 = this.midPrice(this.cfg.leg1);
        var mid2 = this.midPrice(this.cfg.leg2);
        if (mid1 === 0.0 || mid2 === 0.0) return;

        var spreadValue = mid1 - mid2;
        this._updateSpreadHistory(spreadValue);

        if (this.spreadHistory.length < this.cfg.lookback) return;

        var z = this._computeZScore(spreadValue);

        if (this.inPosition) {
            if (this._shouldExit(z)) this._closeSpread();
        } else if (this._shouldEnter(z)) {
            var side = z > 0 ? "sell" : "buy";
            this._openSpread(side);
        }
    }

    _updateSpreadHistory(spreadValue) {
        this.spreadHistory.push(spreadValue);
        if (this.spreadHistory.length > this.cfg.lookback) {
            this.spreadHistory.shift();
        }
    }

    _computeZScore(currentSpread) {
        var n = this.spreadHistory.length;
        if (n === 0) return 0.0;

        var total = 0.0, totalSq = 0.0;
        for (var i = 0; i < n; i++) {
            var s = this.spreadHistory[i];
            total += s;
            totalSq += s * s;
        }

        var mean = total / n;
        var variance = totalSq / n - mean * mean;
        var stddev = Math.sqrt(Math.max(variance, 1e-10));
        return (currentSpread - mean) / stddev;
    }

    _shouldEnter(z) { return Math.abs(z) > this.cfg.entryZ; }

    _shouldExit(z) {
        if (this.isLongSpread) return z > -this.cfg.exitZ;
        return z < this.cfg.exitZ;
    }

    // leg1Side: "buy" means enter long the spread (buy leg1, sell leg2);
    // "sell" means enter short the spread (sell leg1, buy leg2).
    _openSpread(leg1Side) {
        var leg2Qty = this.cfg.size * this.cfg.hedge;
        if (leg1Side === "buy") {
            this.marketBuy({ symbol: this.cfg.leg1, qty: this.cfg.size });
            this.marketSell({ symbol: this.cfg.leg2, qty: leg2Qty });
            this.isLongSpread = true;
        } else {
            this.marketSell({ symbol: this.cfg.leg1, qty: this.cfg.size });
            this.marketBuy({ symbol: this.cfg.leg2, qty: leg2Qty });
            this.isLongSpread = false;
        }
        this.inPosition = true;
        this.tradeCount += 2;
    }

    _closeSpread() {
        var leg2Qty = this.cfg.size * this.cfg.hedge;
        if (this.isLongSpread) {
            this.marketSell({ symbol: this.cfg.leg1, qty: this.cfg.size });
            this.marketBuy({ symbol: this.cfg.leg2, qty: leg2Qty });
        } else {
            this.marketBuy({ symbol: this.cfg.leg1, qty: this.cfg.size });
            this.marketSell({ symbol: this.cfg.leg2, qty: leg2Qty });
        }
        this.inPosition = false;
        this.tradeCount += 2;
    }

    onStart() {
        console.log("PairsStrategy started: leg1=" + this.cfg.leg1 +
                     ", leg2=" + this.cfg.leg2 +
                     ", entryZ=" + this.cfg.entryZ + ", exitZ=" + this.cfg.exitZ);
    }
    onStop() {
        console.log("PairsStrategy stopped: " + this.tradeCount + " trades executed");
    }
}

// ── Smoke test ─────────────────────────────────────────────────────────
//
// flox_js_runner only loads and start()/stop()s a registered strategy; it
// does not feed it data. So this drives the strategy directly: real
// native OrderBook instances stand in for each leg's book (the same mid()
// the engine itself would compute), onBookUpdate is invoked by hand for
// each snapshot the way the engine's book-update dispatch would, and
// marketBuy/marketSell are overridden on the instance to capture emitted
// orders instead of routing through a live engine handle -- the same
// technique tests/test_quickjs.cpp uses to check strategy logic in
// isolation.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}
function near(a, b, eps) { return Math.abs(a - b) <= eps; }

var leg1 = 0, leg2 = 1;
var cfg = { leg1: leg1, leg2: leg2, entryZ: 1.5, exitZ: 0.5, lookback: 10,
            size: 1.0, hedge: 1.0 };

var book1 = new OrderBook(0.01);
var book2 = new OrderBook(0.01);

var strat = new PairsStrategy(cfg);
var signals = [];

strat.marketBuy = function(opts) {
    signals.push({ side: 'buy', symbol: this._resolve(opts.symbol),
                    qty: opts.qty, orderType: 'market' });
};
strat.marketSell = function(opts) {
    signals.push({ side: 'sell', symbol: this._resolve(opts.symbol),
                    qty: opts.qty, orderType: 'market' });
};
strat.midPrice = function(symbol) {
    var book = symbol === leg1 ? book1 : book2;
    var m = book.mid();
    return m === null ? 0.0 : m;
};

// leg2's book is set once and never moves again: mid2 stays 100.0.
book2.applySnapshot([99.99], [1.0], [100.01], [1.0]);
strat.onBookUpdate({}, {});   // fires on the leg2 snapshot too, like the
                              // engine would -- a no-op here since leg1's
                              // book is still empty (mid1 reads 0.0).

// 10-point warmup around spread 5, a jump to 22, then one tick back down.
// Same series as codon/examples/pairs_strategy.codon's driver.
var spreadTargets = [4.0, 6.0, 5.0, 3.0, 7.0, 4.0, 6.0, 5.0, 4.0, 6.0,
                      22.0, 6.0, 5.0, 5.2];
for (var i = 0; i < spreadTargets.length; i++) {
    var mid1 = 100.0 + spreadTargets[i];
    book1.applySnapshot([mid1 - 0.01], [1.0], [mid1 + 0.01], [1.0]);
    strat.onBookUpdate({}, {});
}

check(strat.tradeCount === 4, 'tradeCount (got ' + strat.tradeCount + ', want 4)');
check(signals.length === 4, 'signals emitted (got ' + signals.length + ', want 4)');

if (signals.length === 4) {
    // entry (z > 0, so short the spread: sell leg1, buy leg2), then exit
    // (buy leg1, sell leg2) -- see PairsStrategy._openSpread/_closeSpread.
    var want = [['sell', leg1], ['buy', leg2], ['buy', leg1], ['sell', leg2]];
    for (var j = 0; j < 4; j++) {
        var sig = signals[j];
        check(sig.side === want[j][0],
              'signal[' + j + '].side (got \'' + sig.side + '\', want \'' + want[j][0] + '\')');
        check(sig.symbol === want[j][1],
              'signal[' + j + '].symbol (got ' + sig.symbol + ', want ' + want[j][1] + ')');
        check(sig.orderType === 'market', 'signal[' + j + '].orderType (got \'' + sig.orderType + '\')');
        check(near(sig.qty, 1.0, 1e-9), 'signal[' + j + '].qty (got ' + sig.qty + ', want 1.0)');
    }
}

check(strat.inPosition === false, 'flat after the exit (got inPosition=' + strat.inPosition + ')');
check(strat.isLongSpread === false,
      'isLongSpread left from the short entry (got ' + strat.isLongSpread + ')');

book1.destroy();
book2.destroy();

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('pairs strategy smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
