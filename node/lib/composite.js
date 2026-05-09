// Composite-condition DSL for Node strategies. Mirrors
// python/flox_py/composite.py and quickjs/flox/composite.js.
//
//   const { when, BAR_TYPE_TIME } = require('flox/composite');
//   const fast = when(strat, sym, BAR_TYPE_TIME, M5_NS).ema(50);
//   const slow = when(strat, sym, BAR_TYPE_TIME, M5_NS).ema(200);
//   const crossUp = fast.gt(slow);
//   if (crossUp.isReady() && crossUp.value()) emitMarketBuy();
//
// `strat` must expose `lastNClosedBars(symbol, barType, param, n)` —
// the helper is available across pybind11 / NAPI / QuickJS / Codon.

'use strict';

const BAR_TYPE_TIME = 0;
const BAR_TYPE_TICK = 1;
const BAR_TYPE_VOLUME = 2;
const BAR_TYPE_RENKO = 3;
const BAR_TYPE_RANGE = 4;
const BAR_TYPE_HEIKIN_ASHI = 5;
const BAR_TYPE_BPS_RANGE = 6;

class _Const {
    constructor(v) { this._v = Number(v); }
    isReady() { return true; }
    value() { return this._v; }
    lt(o) { return new _Compare(this, _wrap(o), (a, b) => a < b); }
    le(o) { return new _Compare(this, _wrap(o), (a, b) => a <= b); }
    gt(o) { return new _Compare(this, _wrap(o), (a, b) => a > b); }
    ge(o) { return new _Compare(this, _wrap(o), (a, b) => a >= b); }
    eq(o) { return new _Compare(this, _wrap(o), (a, b) => a === b); }
    ne(o) { return new _Compare(this, _wrap(o), (a, b) => a !== b); }
}

function _wrap(x) {
    if (x && typeof x.isReady === 'function' && typeof x.value === 'function') return x;
    return new _Const(Number(x));
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
        const bars = this._bars();
        if (bars.length < this._needed()) return NaN;
        const sliceStart = Math.max(0, bars.length - this._n - (this._kind === 'rsi' ? 1 : 0));
        const closes = [];
        for (let i = sliceStart; i < bars.length; i++) closes.push(bars[i].close);
        if (this._kind === 'sma') {
            const tail = closes.slice(-this._n);
            return tail.reduce((a, b) => a + b, 0) / this._n;
        }
        if (this._kind === 'ema') {
            const alpha = 2.0 / (this._n + 1.0);
            let v = closes[0];
            for (let k = 1; k < closes.length; k++) v = alpha * closes[k] + (1 - alpha) * v;
            return v;
        }
        if (this._kind === 'rsi') {
            let gains = 0, losses = 0;
            for (let m = 1; m < closes.length; m++) {
                const d = closes[m] - closes[m - 1];
                if (d >= 0) gains += d; else losses -= d;
            }
            const avgG = gains / this._n;
            const avgL = losses / this._n;
            if (avgL === 0) return 100.0;
            const rs = avgG / avgL;
            return 100.0 - (100.0 / (1.0 + rs));
        }
        if (this._kind === 'close') return closes[closes.length - 1];
        throw new Error(`unknown indicator kind: ${this._kind}`);
    }
    lt(o) { return new _Compare(this, _wrap(o), (a, b) => a < b); }
    le(o) { return new _Compare(this, _wrap(o), (a, b) => a <= b); }
    gt(o) { return new _Compare(this, _wrap(o), (a, b) => a > b); }
    ge(o) { return new _Compare(this, _wrap(o), (a, b) => a >= b); }
    eq(o) { return new _Compare(this, _wrap(o), (a, b) => a === b); }
    ne(o) { return new _Compare(this, _wrap(o), (a, b) => a !== b); }
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

function _gridKey(sym, bt, param) { return `${sym}|${bt}|${param}`; }

class _IndicatorGrid {
    constructor() { this._cells = Object.create(null); this._keys = []; }
    _set(sym, bt, param, ind) {
        const key = _gridKey(sym, bt, param);
        this._cells[key] = ind;
        this._keys.push({ symbol: sym, barType: bt, param });
    }
    get(sym, bt, param) { return this._cells[_gridKey(sym, bt, param)]; }
    keys() { return this._keys.slice(); }
    size() { return this._keys.length; }
}

class _GridBuilder {
    constructor(strategy, symbols, timeframes) {
        this._s = strategy;
        this._symbols = symbols.slice();
        this._tfs = timeframes.map((tf) =>
            Array.isArray(tf) && tf.length === 2
                ? { bt: Number(tf[0]), param: Number(tf[1]) }
                : { bt: BAR_TYPE_TIME, param: Number(tf) });
    }
    _build(period, kind) {
        const g = new _IndicatorGrid();
        for (const sym of this._symbols) {
            for (const tf of this._tfs) {
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

module.exports = {
    when,
    grid,
    BAR_TYPE_TIME, BAR_TYPE_TICK, BAR_TYPE_VOLUME,
    BAR_TYPE_RENKO, BAR_TYPE_RANGE, BAR_TYPE_HEIKIN_ASHI, BAR_TYPE_BPS_RANGE,
};
