/**
 * RSI Mean-Reversion Strategy -- real end-to-end example.
 *
 * Uses RSI + Bollinger Bands for entry, ATR for position sizing,
 * with trailing stop for risk management.
 *
 * Usage:
 *     ./build/src/quickjs/flox_js_runner examples/js_rsi_strategy.js
 */

class RsiMeanReversion extends Strategy {
    constructor() {
        super({ exchange: "Backtest", symbols: ["BTCUSDT"] });

        this.rsi = new RSI(14);
        this.bb = new Bollinger(20, 2.0);
        this.atr = new ATR(14);
        this.fastEma = new EMA(10);
        this.slowEma = new EMA(30);

        this.tradeCount = 0;
        this.wins = 0;
        this.losses = 0;
        this.totalPnl = 0;
        this.entryPrice = 0;
        this.positionSide = null;  // "long" or "short" or null
        this.trailingStopId = 0;
        this.barCount = 0;

        // Track high/low for ATR
        this.prevClose = 0;
    }

    onTrade(ctx, trade) {
        var price = trade.price;
        this.barCount++;

        // Update indicators
        var rsiVal = this.rsi.update(price);
        this.bb.update(price);
        var atrVal = this.atr.update(price, price * 0.999, price);  // approx H/L from trade
        var fastVal = this.fastEma.update(price);
        var slowVal = this.slowEma.update(price);

        if (!this.rsi.ready || !this.bb.ready || !this.atr.ready) {
            this.prevClose = price;
            return;
        }

        // Position sizing: risk 1% of notional per ATR unit
        var riskPerUnit = atrVal * 2;
        var qty = riskPerUnit > 0 ? Math.max(0.001, Math.min(1.0, 100.0 / riskPerUnit)) : 0.01;
        qty = Math.round(qty * 1000) / 1000;  // round to 3 decimals

        // Track existing position PnL
        if (this.positionSide === "long" && ctx.position <= 0) {
            // Position was closed (by trailing stop)
            var pnl = price - this.entryPrice;
            this.totalPnl += pnl;
            if (pnl > 0) this.wins++;
            else this.losses++;
            this.positionSide = null;
            this.tradeCount++;
        } else if (this.positionSide === "short" && ctx.position >= 0) {
            var pnl = this.entryPrice - price;
            this.totalPnl += pnl;
            if (pnl > 0) this.wins++;
            else this.losses++;
            this.positionSide = null;
            this.tradeCount++;
        }

        // Entry logic: RSI oversold/overbought + price at Bollinger band + EMA trend filter
        if (this.positionSide === null) {
            // Long: RSI oversold + price near lower BB + fast EMA above slow (uptrend)
            if (rsiVal < 30 && price < this.bb.lower * 1.005 && fastVal > slowVal) {
                this.marketBuy({ qty: qty });
                this.entryPrice = price;
                this.positionSide = "long";

                // Set trailing stop at 2x ATR
                var stopOffset = atrVal * 2;
                this.trailingStopId = this.trailingStop({
                    side: "sell", offset: stopOffset, qty: qty
                });
            }
            // Short: RSI overbought + price near upper BB + fast EMA below slow (downtrend)
            else if (rsiVal > 70 && price > this.bb.upper * 0.995 && fastVal < slowVal) {
                this.marketSell({ qty: qty });
                this.entryPrice = price;
                this.positionSide = "short";

                var stopOffset = atrVal * 2;
                this.trailingStopId = this.trailingStop({
                    side: "buy", offset: stopOffset, qty: qty
                });
            }
        }

        this.prevClose = price;
    }

    onStart() {
        console.log("RsiMeanReversion started");
        console.log("  Symbols:", JSON.stringify(this.symbols));
        console.log("  RSI(14) + BB(20,2) + ATR(14) + EMA(10/30)");
    }

    onStop() {
        var winRate = this.tradeCount > 0 ? (this.wins / this.tradeCount * 100).toFixed(1) : "0.0";
        console.log("\n=== Strategy Results ===");
        console.log("  Bars processed:  " + this.barCount);
        console.log("  Total trades:    " + this.tradeCount);
        console.log("  Wins:            " + this.wins);
        console.log("  Losses:          " + this.losses);
        console.log("  Win rate:        " + winRate + "%");
        console.log("  Total PnL:       " + this.totalPnl.toFixed(2));
    }
}

flox.register(new RsiMeanReversion());
