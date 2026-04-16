var SIDE_MAP = { buy: 0, sell: 1 };
var TIF_MAP = { GTC: 0, IOC: 1, FOK: 2, GTD: 3, POST_ONLY: 4 };

function _resolveSide(side) {
    var v = SIDE_MAP[side];
    if (v === undefined) throw new Error("Invalid side: '" + side + "'. Use 'buy' or 'sell'.");
    return v;
}

function _resolveTif(tif) {
    if (tif === undefined || tif === null) return 0;
    var v = TIF_MAP[tif];
    if (v === undefined) throw new Error("Invalid tif: '" + tif + "'. Use GTC/IOC/FOK/POST_ONLY.");
    return v;
}

class Strategy {
    constructor(config) {
        this._exchange = config.exchange || null;
        this._symbolNames = config.symbols;
        this._symbolMap = {};
        this._reverseMap = {};
        this._handle = null;
    }

    onTrade(ctx, trade) {}
    onBookUpdate(ctx, book) {}
    onStart() {}
    onStop() {}

    // --- Market orders ---

    marketBuy(opts) {
        return __flox_emit_market_buy(this._handle, this._resolve(opts.symbol), opts.qty);
    }
    marketSell(opts) {
        return __flox_emit_market_sell(this._handle, this._resolve(opts.symbol), opts.qty);
    }

    // --- Limit orders ---

    limitBuy(opts) {
        var sym = this._resolve(opts.symbol);
        var tif = _resolveTif(opts.tif);
        return __flox_emit_limit_buy_tif(this._handle, sym, opts.price, opts.qty, tif);
    }
    limitSell(opts) {
        var sym = this._resolve(opts.symbol);
        var tif = _resolveTif(opts.tif);
        return __flox_emit_limit_sell_tif(this._handle, sym, opts.price, opts.qty, tif);
    }

    // --- Stop orders ---

    stopMarket(opts) {
        return __flox_emit_stop_market(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.trigger, opts.qty);
    }
    stopLimit(opts) {
        return __flox_emit_stop_limit(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.trigger, opts.price, opts.qty);
    }

    // --- Take profit orders ---

    takeProfitMarket(opts) {
        return __flox_emit_take_profit_market(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.trigger, opts.qty);
    }
    takeProfitLimit(opts) {
        return __flox_emit_take_profit_limit(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.trigger, opts.price, opts.qty);
    }

    // --- Trailing stop orders ---

    trailingStop(opts) {
        return __flox_emit_trailing_stop(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.offset, opts.qty);
    }
    trailingStopPercent(opts) {
        return __flox_emit_trailing_stop_pct(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.callbackBps, opts.qty);
    }

    // --- Order management ---

    cancel(orderId) { __flox_emit_cancel(this._handle, orderId); }
    cancelAll(symbol) { __flox_emit_cancel_all(this._handle, this._resolve(symbol)); }
    modify(orderId, opts) {
        __flox_emit_modify(this._handle, orderId, opts.price, opts.qty);
    }
    closePosition(symbol) {
        return __flox_emit_close_position(this._handle, this._resolve(symbol));
    }

    // --- Context queries ---

    position(symbol) { return __flox_position(this._handle, this._resolve(symbol)); }
    lastPrice(symbol) { return __flox_last_trade_price(this._handle, this._resolve(symbol)); }
    bestBid(symbol) { return __flox_best_bid(this._handle, this._resolve(symbol)); }
    bestAsk(symbol) { return __flox_best_ask(this._handle, this._resolve(symbol)); }
    midPrice(symbol) { return __flox_mid_price(this._handle, this._resolve(symbol)); }
    orderStatus(orderId) { return __flox_get_order_status(this._handle, orderId); }

    // --- Properties ---

    get primarySymbol() { return this._symbolNames[0]; }
    get hasPosition() { return this.position() !== 0; }
    get symbols() { return this._symbolNames; }

    // --- Internal ---

    _resolve(symbol) {
        var name = symbol !== undefined && symbol !== null ? symbol : this._symbolNames[0];
        var id = this._symbolMap[name];
        if (id === undefined) {
            throw new Error("Unknown symbol: " + name);
        }
        return id;
    }

    _dispatchTrade(rawCtx, rawTrade) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        rawTrade.symbol = this._reverseMap[rawTrade.symbolId] || "";
        rawTrade.side = rawTrade.isBuy ? "buy" : "sell";
        this.onTrade(rawCtx, rawTrade);
    }

    _dispatchBook(rawCtx, rawBook) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        rawBook.symbol = this._reverseMap[rawBook.symbolId] || "";
        this.onBookUpdate(rawCtx, rawBook);
    }
}
