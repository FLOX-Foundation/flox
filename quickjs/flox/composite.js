// Composite-condition DSL. Mirrors python/flox_py/composite.py and
// node/lib/composite.js so a QuickJS strategy can write
//   when(strat, sym, BAR_TYPE_TIME, H4_NS).ema(50).gt(
//     when(strat, sym, BAR_TYPE_TIME, H4_NS).ema(200))
// and compose conditions with .and / .or / .not.
//
// The DSL sits on top of `Strategy.lastNClosedBars(symbol, barType,
// param, n)` (W1-T026 / T027). No engine state, no C ABI surface.

var BAR_TYPE_TIME = 0;
var BAR_TYPE_TICK = 1;
var BAR_TYPE_VOLUME = 2;
var BAR_TYPE_RENKO = 3;
var BAR_TYPE_RANGE = 4;
var BAR_TYPE_HEIKIN_ASHI = 5;
var BAR_TYPE_BPS_RANGE = 6;

function _wrapConst(x) {
    if (x && typeof x.isReady === 'function' && typeof x.value === 'function') {
        return x;
    }
    return new _Const(Number(x));
}

class _Const {
    constructor(v) { this._v = Number(v); }
    isReady() { return true; }
    value() { return this._v; }
    lt(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a < b; }); }
    le(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a <= b; }); }
    gt(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a > b; }); }
    ge(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a >= b; }); }
    eq(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a === b; }); }
    ne(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a !== b; }); }
}

class _Indicator {
    constructor(strategy, symbol, barType, param, period, kind) {
        this._s = strategy;
        this._sym = symbol;
        this._bt = barType;
        this._param = param;
        this._n = period;
        this._kind = kind;
    }
    _needed() { return this._kind === 'rsi' ? this._n + 1 : this._n; }
    _bars() { return this._s.lastNClosedBars(this._sym, this._bt, this._param, this._needed()); }
    isReady() { return this._bars().length >= this._needed(); }
    value() {
        var bars = this._bars();
        if (bars.length < this._needed()) return NaN;
        var sliceStart = bars.length - this._n - (this._kind === 'rsi' ? 1 : 0);
        if (sliceStart < 0) sliceStart = 0;
        var closes = [];
        for (var i = sliceStart; i < bars.length; i++) closes.push(bars[i].close);
        if (this._kind === 'sma') {
            var sum = 0;
            var tail = closes.slice(closes.length - this._n);
            for (var j = 0; j < tail.length; j++) sum += tail[j];
            return sum / this._n;
        }
        if (this._kind === 'ema') {
            var alpha = 2.0 / (this._n + 1.0);
            var v = closes[0];
            for (var k = 1; k < closes.length; k++) {
                v = alpha * closes[k] + (1 - alpha) * v;
            }
            return v;
        }
        if (this._kind === 'rsi') {
            var gains = 0, losses = 0;
            for (var m = 1; m < closes.length; m++) {
                var d = closes[m] - closes[m - 1];
                if (d >= 0) gains += d; else losses -= d;
            }
            var avgG = gains / this._n;
            var avgL = losses / this._n;
            if (avgL === 0) return 100.0;
            var rs = avgG / avgL;
            return 100.0 - (100.0 / (1.0 + rs));
        }
        if (this._kind === 'close') return closes[closes.length - 1];
        throw new Error('unknown indicator kind: ' + this._kind);
    }
    lt(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a < b; }); }
    le(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a <= b; }); }
    gt(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a > b; }); }
    ge(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a >= b; }); }
    eq(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a === b; }); }
    ne(o) { return new _Compare(this, _wrapConst(o), function(a, b){ return a !== b; }); }
}

class _TfHandle {
    constructor(strategy, symbol, barType, param) {
        this._s = strategy;
        this._sym = symbol;
        this._bt = barType;
        this._param = param;
    }
    sma(period) { return new _Indicator(this._s, this._sym, this._bt, this._param, period, 'sma'); }
    ema(period) { return new _Indicator(this._s, this._sym, this._bt, this._param, period, 'ema'); }
    rsi(period) { return new _Indicator(this._s, this._sym, this._bt, this._param, period, 'rsi'); }
    close() { return new _Indicator(this._s, this._sym, this._bt, this._param, 1, 'close'); }
}

function when(strategy, symbol, barType, param) {
    return new _TfHandle(strategy, symbol, barType, param);
}

class _Condition {
    isReady() { throw new Error('abstract'); }
    value() { throw new Error('abstract'); }
    and(other) { return new _And(this, other); }
    or(other) { return new _Or(this, other); }
    not() { return new _Not(this); }
}

class _Compare extends _Condition {
    constructor(lhs, rhs, op) { super(); this._l = lhs; this._r = rhs; this._op = op; }
    isReady() { return this._l.isReady() && this._r.isReady(); }
    value() { return Boolean(this._op(this._l.value(), this._r.value())); }
}

class _And extends _Condition {
    constructor(l, r) { super(); this._l = l; this._r = r; }
    isReady() { return this._l.isReady() && this._r.isReady(); }
    value() { return Boolean(this._l.value() && this._r.value()); }
}

class _Or extends _Condition {
    constructor(l, r) { super(); this._l = l; this._r = r; }
    isReady() { return this._l.isReady() && this._r.isReady(); }
    value() { return Boolean(this._l.value() || this._r.value()); }
}

class _Not extends _Condition {
    constructor(inner) { super(); this._inner = inner; }
    isReady() { return this._inner.isReady(); }
    value() { return !Boolean(this._inner.value()); }
}

// ---------------------------------------------------------------------------
// Indicator-grid sugar: instantiate the same indicator across a cross-product
// of symbols and timeframes in one declaration. Mirrors Python `composite.grid`.
// ---------------------------------------------------------------------------

function _gridKey(sym, bt, param) {
    return sym + '|' + bt + '|' + param;
}

class _IndicatorGrid {
    constructor() { this._cells = Object.create(null); this._keys = []; }
    _set(sym, bt, param, ind) {
        var key = _gridKey(sym, bt, param);
        this._cells[key] = ind;
        this._keys.push({ symbol: sym, barType: bt, param: param });
    }
    get(sym, bt, param) {
        return this._cells[_gridKey(sym, bt, param)];
    }
    keys() { return this._keys.slice(); }
    size() { return this._keys.length; }
}

class _GridBuilder {
    constructor(strategy, symbols, timeframes) {
        this._s = strategy;
        this._symbols = symbols.slice();
        this._tfs = [];
        for (var i = 0; i < timeframes.length; i++) {
            var tf = timeframes[i];
            if (Array.isArray(tf) && tf.length === 2) {
                this._tfs.push({ bt: Number(tf[0]), param: Number(tf[1]) });
            } else {
                this._tfs.push({ bt: BAR_TYPE_TIME, param: Number(tf) });
            }
        }
    }
    _build(period, kind) {
        var g = new _IndicatorGrid();
        for (var i = 0; i < this._symbols.length; i++) {
            for (var j = 0; j < this._tfs.length; j++) {
                var sym = this._symbols[i];
                var tf = this._tfs[j];
                g._set(sym, tf.bt, tf.param,
                       new _Indicator(this._s, sym, tf.bt, tf.param, period, kind));
            }
        }
        return g;
    }
    sma(period) { return this._build(period, 'sma'); }
    ema(period) { return this._build(period, 'ema'); }
    rsi(period) { return this._build(period, 'rsi'); }
    close() { return this._build(1, 'close'); }
}

function grid(strategy, symbols, timeframes) {
    return new _GridBuilder(strategy, symbols, timeframes);
}
