// quickjs/flox/indicators.js
//
// Each indicator is ONE class with both batch and streaming API on the
// same instance. No own ring buffer / alpha / sum / count — streaming
// works by accumulating history and re-running the same C-API batch
// function. Parity with .compute() is by construction.
//
//   const ema = new EMA(10);
//   const out = ema.compute(prices);          // batch
//   for (const v of stream) {                 // streaming on the same instance
//     ema.update(v);
//     if (ema.ready) console.log(ema.value);
//   }

// ── Helpers ────────────────────────────────────────────────────────────

function _last(arr) {
    if (!arr || arr.length === 0) return NaN;
    return arr[arr.length - 1];
}

function _optNum(v) { return (typeof v !== 'number' || isNaN(v)) ? null : v; }

// Single-input streaming pattern: history + re-run batch on full history.
class _Stream1 {
    constructor(period, batchFn) {
        this._period = period;
        this._batchFn = batchFn;
        this._history = [];
    }
    update(v) {
        this._history.push(v);
        return _optNum(_last(this._batchFn(this._history, this._period)));
    }
    get value() {
        if (this._history.length === 0) return null;
        return _optNum(_last(this._batchFn(this._history, this._period)));
    }
    get ready() { return this._history.length >= this._period; }
    get count() { return this._history.length; }
    reset() { this._history = []; }
}

// Bar-input streaming (high, low, close).
class _StreamBar {
    constructor(period, batchFn) {
        this._period = period;
        this._batchFn = batchFn;
        this._h = []; this._l = []; this._c = [];
    }
    update(high, low, close) {
        this._h.push(high); this._l.push(low); this._c.push(close);
        return _optNum(_last(this._batchFn(this._h, this._l, this._c, this._period)));
    }
    get value() {
        if (this._h.length === 0) return null;
        return _optNum(_last(this._batchFn(this._h, this._l, this._c, this._period)));
    }
    get ready() { return this._h.length >= this._period; }
    get count() { return this._h.length; }
    reset() { this._h = []; this._l = []; this._c = []; }
}

// HighLow streaming (high, low).
class _StreamHighLow {
    constructor(period, batchFn) {
        this._period = period;
        this._batchFn = batchFn;
        this._h = []; this._l = [];
    }
    update(high, low) {
        this._h.push(high); this._l.push(low);
        return _optNum(_last(this._batchFn(this._h, this._l, this._period)));
    }
    get value() {
        if (this._h.length === 0) return null;
        return _optNum(_last(this._batchFn(this._h, this._l, this._period)));
    }
    get ready() { return this._h.length >= this._period; }
    get count() { return this._h.length; }
    reset() { this._h = []; this._l = []; }
}

// OHLC streaming.
class _StreamOhlc {
    constructor(period, batchFn) {
        this._period = period;
        this._batchFn = batchFn;
        this._o = []; this._h = []; this._l = []; this._c = [];
    }
    update(open, high, low, close) {
        this._o.push(open); this._h.push(high); this._l.push(low); this._c.push(close);
        return _optNum(_last(this._batchFn(this._o, this._h, this._l, this._c, this._period)));
    }
    get value() {
        if (this._o.length === 0) return null;
        return _optNum(_last(this._batchFn(this._o, this._h, this._l, this._c, this._period)));
    }
    get ready() { return this._o.length >= this._period; }
    get count() { return this._o.length; }
    reset() { this._o = []; this._h = []; this._l = []; this._c = []; }
}

// Pair streaming (x, y).
class _StreamPair {
    constructor(period, batchFn) {
        this._period = period;
        this._batchFn = batchFn;
        this._x = []; this._y = [];
    }
    update(x, y) {
        this._x.push(x); this._y.push(y);
        return _optNum(_last(this._batchFn(this._x, this._y, this._period)));
    }
    get value() {
        if (this._x.length === 0) return null;
        return _optNum(_last(this._batchFn(this._x, this._y, this._period)));
    }
    get ready() { return this._x.length >= this._period; }
    get count() { return this._x.length; }
    reset() { this._x = []; this._y = []; }
}

// ============================================================
// Moving averages / oscillators
// ============================================================

class SMA extends _Stream1 {
    constructor(period) { super(period, __flox_indicator_sma); }
    static compute(data, period) { return __flox_indicator_sma(data, period); }
}

class EMA extends _Stream1 {
    constructor(period) { super(period, __flox_indicator_ema); }
    static compute(data, period) { return __flox_indicator_ema(data, period); }
}

class RMA extends _Stream1 {
    constructor(period) { super(period, __flox_indicator_rma); }
    static compute(data, period) { return __flox_indicator_rma(data, period); }
}

class DEMA extends _Stream1 {
    constructor(period) { super(period, __flox_indicator_dema); }
    static compute(data, period) { return __flox_indicator_dema(data, period); }
}

class TEMA extends _Stream1 {
    constructor(period) { super(period, __flox_indicator_tema); }
    static compute(data, period) { return __flox_indicator_tema(data, period); }
}

class RSI extends _Stream1 {
    constructor(period) { super(period, __flox_indicator_rsi); }
    static compute(data, period) { return __flox_indicator_rsi(data, period); }
}

class Slope extends _Stream1 {
    constructor(length) { super(length, __flox_indicator_slope); }
    static compute(data, length) { return __flox_indicator_slope(data, length); }
}

// KAMA(period, fast=2, slow=30) — extra ctor args, custom adapter
class KAMA {
    constructor(period, fast = 2, slow = 30) {
        this._period = period; this._fast = fast; this._slow = slow;
        this._history = [];
    }
    update(v) {
        this._history.push(v);
        return _optNum(_last(__flox_indicator_kama(this._history, this._period, this._fast, this._slow)));
    }
    get value() {
        if (this._history.length === 0) return null;
        return _optNum(_last(__flox_indicator_kama(this._history, this._period, this._fast, this._slow)));
    }
    get ready() { return this._history.length > this._period; }
    get count() { return this._history.length; }
    reset() { this._history = []; }
    compute(data) { return __flox_indicator_kama(data, this._period, this._fast, this._slow); }
    static compute(data, period, fast = 2, slow = 30) {
        return __flox_indicator_kama(data, period, fast, slow);
    }
}

// ============================================================
// Bar-input
// ============================================================

class ATR extends _StreamBar {
    constructor(period) { super(period, __flox_indicator_atr); }
    static compute(high, low, close, period) {
        return __flox_indicator_atr(high, low, close, period);
    }
}

class CCI extends _StreamBar {
    constructor(period) { super(period, __flox_indicator_cci); }
    static compute(high, low, close, period) {
        return __flox_indicator_cci(high, low, close, period);
    }
}

class CHOP extends _StreamBar {
    constructor(period) { super(period, __flox_indicator_chop); }
    static compute(high, low, close, period) {
        return __flox_indicator_chop(high, low, close, period);
    }
}

// ADX returns a struct {adx, plus_di, minus_di} — keep batch-only
class ADX {
    static compute(high, low, close, period) {
        return __flox_indicator_adx(high, low, close, period);
    }
}

// ============================================================
// Multi-output (line/signal/histogram, upper/middle/lower, k/d)
// ============================================================

// MACD(fast, slow, signal) — multi-output
class MACD {
    constructor(fast = 12, slow = 26, signal = 9) {
        this._fast = fast; this._slow = slow; this._signal = signal;
        this._history = [];
    }
    update(v) {
        this._history.push(v);
        const r = __flox_indicator_macd(this._history, this._fast, this._slow, this._signal);
        return _optNum(_last(r.line));
    }
    get value() {
        if (this._history.length === 0) return null;
        const r = __flox_indicator_macd(this._history, this._fast, this._slow, this._signal);
        return _optNum(_last(r.line));
    }
    get line() { return this.value; }
    get signal() {
        if (this._history.length === 0) return null;
        const r = __flox_indicator_macd(this._history, this._fast, this._slow, this._signal);
        return _optNum(_last(r.signal));
    }
    get histogram() {
        if (this._history.length === 0) return null;
        const r = __flox_indicator_macd(this._history, this._fast, this._slow, this._signal);
        return _optNum(_last(r.histogram));
    }
    get ready() {
        return this._history.length >= this._slow + this._signal - 1;
    }
    get count() { return this._history.length; }
    reset() { this._history = []; }
    compute(data) { return __flox_indicator_macd(data, this._fast, this._slow, this._signal); }
    static compute(data, fast = 12, slow = 26, signal = 9) {
        return __flox_indicator_macd(data, fast, slow, signal);
    }
}

class Bollinger {
    constructor(period = 20, multiplier = 2.0) {
        this._period = period; this._mul = multiplier;
        this._history = [];
    }
    update(v) {
        this._history.push(v);
        const r = __flox_indicator_bollinger(this._history, this._period, this._mul);
        return _optNum(_last(r.middle));
    }
    get value() { return this.middle; }
    get middle() {
        if (this._history.length === 0) return null;
        return _optNum(_last(__flox_indicator_bollinger(this._history, this._period, this._mul).middle));
    }
    get upper() {
        if (this._history.length === 0) return null;
        return _optNum(_last(__flox_indicator_bollinger(this._history, this._period, this._mul).upper));
    }
    get lower() {
        if (this._history.length === 0) return null;
        return _optNum(_last(__flox_indicator_bollinger(this._history, this._period, this._mul).lower));
    }
    get ready() { return this._history.length >= this._period; }
    get count() { return this._history.length; }
    reset() { this._history = []; }
    compute(data) { return __flox_indicator_bollinger(data, this._period, this._mul); }
    static compute(data, period = 20, multiplier = 2.0) {
        return __flox_indicator_bollinger(data, period, multiplier);
    }
}

class Stochastic {
    constructor(kPeriod = 14, dPeriod = 3) {
        this._k = kPeriod; this._d = dPeriod;
        this._h = []; this._l = []; this._c = [];
    }
    update(high, low, close) {
        this._h.push(high); this._l.push(low); this._c.push(close);
        const r = __flox_indicator_stochastic(this._h, this._l, this._c, this._k, this._d);
        return _optNum(_last(r.k));
    }
    get value() { return this.k; }
    get k() {
        if (this._h.length === 0) return null;
        return _optNum(_last(__flox_indicator_stochastic(this._h, this._l, this._c, this._k, this._d).k));
    }
    get d() {
        if (this._h.length === 0) return null;
        return _optNum(_last(__flox_indicator_stochastic(this._h, this._l, this._c, this._k, this._d).d));
    }
    get ready() { return this._h.length >= this._k; }
    get count() { return this._h.length; }
    reset() { this._h = []; this._l = []; this._c = []; }
    compute(high, low, close) { return __flox_indicator_stochastic(high, low, close, this._k, this._d); }
    static compute(high, low, close, kPeriod = 14, dPeriod = 3) {
        return __flox_indicator_stochastic(high, low, close, kPeriod, dPeriod);
    }
}

// ============================================================
// Volume / cumulative — batch-only static helpers
// ============================================================

class OBV {
    static compute(close, volume) { return __flox_indicator_obv(close, volume); }
}
class VWAP {
    static compute(close, volume, window) { return __flox_indicator_vwap(close, volume, window); }
}
class CVD {
    static compute(open, high, low, close, volume) {
        return __flox_indicator_cvd(open, high, low, close, volume);
    }
}

// ============================================================
// Statistical / volatility
// ============================================================

class Skewness extends _Stream1 {
    constructor(period) { super(period, __flox_indicator_skewness); }
    static compute(data, period) { return __flox_indicator_skewness(data, period); }
}

class Kurtosis extends _Stream1 {
    constructor(period) { super(period, __flox_indicator_kurtosis); }
    static compute(data, period) { return __flox_indicator_kurtosis(data, period); }
}

class RollingZScore extends _Stream1 {
    constructor(period) { super(period, __flox_indicator_rolling_zscore); }
    static compute(data, period) { return __flox_indicator_rolling_zscore(data, period); }
}

// ShannonEntropy(period, bins)
class ShannonEntropy {
    constructor(period, bins = 10) { this._period = period; this._bins = bins; this._history = []; }
    update(v) {
        this._history.push(v);
        return _optNum(_last(__flox_indicator_shannon_entropy(this._history, this._period, this._bins)));
    }
    get value() {
        if (this._history.length === 0) return null;
        return _optNum(_last(__flox_indicator_shannon_entropy(this._history, this._period, this._bins)));
    }
    get ready() { return this._history.length >= this._period; }
    get count() { return this._history.length; }
    reset() { this._history = []; }
    compute(data) { return __flox_indicator_shannon_entropy(data, this._period, this._bins); }
    static compute(data, period, bins = 10) {
        return __flox_indicator_shannon_entropy(data, period, bins);
    }
}

class ParkinsonVol extends _StreamHighLow {
    constructor(period) { super(period, __flox_indicator_parkinson_vol); }
    static compute(high, low, period) { return __flox_indicator_parkinson_vol(high, low, period); }
}

class RogersSatchellVol extends _StreamOhlc {
    constructor(period) { super(period, __flox_indicator_rogers_satchell_vol); }
    static compute(open, high, low, close, period) {
        return __flox_indicator_rogers_satchell_vol(open, high, low, close, period);
    }
}

class Correlation extends _StreamPair {
    constructor(period) { super(period, __flox_indicator_correlation); }
    static compute(x, y, period) { return __flox_indicator_correlation(x, y, period); }
}

// ============================================================
// Stationarity tests
// ============================================================

// Augmented Dickey-Fuller test. Returns { test_stat, p_value, used_lag }.
function adf(data, max_lag, regression) {
    return __flox_indicator_adf(data, max_lag === undefined ? 4 : max_lag,
                                regression === undefined ? "c" : regression);
}

// ============================================================
// AutoCorrelation
// ============================================================

class AutoCorrelation {
    constructor(window, lag) { this._window = window; this._lag = lag; this._history = []; }
    update(v) {
        this._history.push(v);
        return _optNum(_last(__flox_indicator_autocorrelation(this._history, this._window, this._lag)));
    }
    get value() {
        if (this._history.length === 0) return null;
        return _optNum(_last(__flox_indicator_autocorrelation(this._history, this._window, this._lag)));
    }
    get ready() { return this._history.length >= this._window + this._lag; }
    get count() { return this._history.length; }
    reset() { this._history = []; }
    compute(data) { return __flox_indicator_autocorrelation(data, this._window, this._lag); }
    static compute(data, window, lag) { return __flox_indicator_autocorrelation(data, window, lag); }
}
