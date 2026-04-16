class SmaCrossover extends Strategy {
    constructor() {
        super({ exchange: "Binance", symbols: ["BTCUSDT"] });
        this.fastSma = new SMA(10);
        this.slowSma = new SMA(30);
        this.orderSize = 1.0;
        this.inLong = false;
        this.inShort = false;
        this.prevFastAbove = false;
    }

    onTrade(ctx, trade) {
        var fast = this.fastSma.update(trade.price);
        var slow = this.slowSma.update(trade.price);
        if (!this.slowSma.ready) return;

        var fastAbove = fast > slow;

        if (fastAbove && !this.prevFastAbove && !this.inLong) {
            if (this.inShort) {
                this.marketBuy({ qty: this.orderSize });
                this.inShort = false;
            }
            this.marketBuy({ qty: this.orderSize });
            this.inLong = true;
        } else if (!fastAbove && this.prevFastAbove && !this.inShort) {
            if (this.inLong) {
                this.marketSell({ qty: this.orderSize });
                this.inLong = false;
            }
            this.marketSell({ qty: this.orderSize });
            this.inShort = true;
        }

        this.prevFastAbove = fastAbove;
    }

    onStart() { console.log("SmaCrossover started on " + this.primarySymbol); }
    onStop() { console.log("SmaCrossover stopped"); }
}

flox.register(new SmaCrossover());
