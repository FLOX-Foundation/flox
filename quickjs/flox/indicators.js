// ============================================================
// Moving Averages
// ============================================================

class SMA {
    constructor(period) {
        this._period = period;
        this._buffer = new Array(period);
        this._sum = 0;
        this._count = 0;
        this._index = 0;
        this._value = 0;
    }
    update(value) {
        if (this._count < this._period) {
            this._buffer[this._count] = value;
            this._sum += value;
            this._count++;
        } else {
            this._sum -= this._buffer[this._index];
            this._buffer[this._index] = value;
            this._sum += value;
            this._index = (this._index + 1) % this._period;
        }
        this._value = this._sum / this._count;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._count >= this._period; }
    reset() { this._buffer = new Array(this._period); this._sum = 0; this._count = 0; this._index = 0; this._value = 0; }
    static compute(data, period) { return __flox_indicator_sma(data, period); }
}

class EMA {
    constructor(period) {
        this._period = period;
        this._multiplier = 2.0 / (period + 1);
        this._value = 0;
        this._count = 0;
        this._sum = 0;
    }
    update(value) {
        if (this._count < this._period) {
            this._sum += value;
            this._count++;
            this._value = this._sum / this._count;
        } else {
            this._value = (value - this._value) * this._multiplier + this._value;
        }
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._count >= this._period; }
    reset() { this._value = 0; this._count = 0; this._sum = 0; }
    static compute(data, period) { return __flox_indicator_ema(data, period); }
}

class RMA {
    constructor(period) {
        this._period = period;
        this._alpha = 1.0 / period;
        this._value = 0;
        this._count = 0;
        this._sum = 0;
    }
    update(value) {
        if (this._count < this._period) {
            this._sum += value;
            this._count++;
            this._value = this._sum / this._count;
        } else {
            this._value = this._alpha * value + (1 - this._alpha) * this._value;
        }
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._count >= this._period; }
    reset() { this._value = 0; this._count = 0; this._sum = 0; }
    static compute(data, period) { return __flox_indicator_rma(data, period); }
}

class DEMA {
    constructor(period) {
        this._ema1 = new EMA(period);
        this._ema2 = new EMA(period);
        this._value = 0;
    }
    update(value) {
        var e1 = this._ema1.update(value);
        var e2 = this._ema2.update(e1);
        this._value = 2 * e1 - e2;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._ema2.ready; }
    reset() { this._ema1.reset(); this._ema2.reset(); this._value = 0; }
    static compute(data, period) { return __flox_indicator_dema(data, period); }
}

class TEMA {
    constructor(period) {
        this._ema1 = new EMA(period);
        this._ema2 = new EMA(period);
        this._ema3 = new EMA(period);
        this._value = 0;
    }
    update(value) {
        var e1 = this._ema1.update(value);
        var e2 = this._ema2.update(e1);
        var e3 = this._ema3.update(e2);
        this._value = 3 * e1 - 3 * e2 + e3;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._ema3.ready; }
    reset() { this._ema1.reset(); this._ema2.reset(); this._ema3.reset(); this._value = 0; }
    static compute(data, period) { return __flox_indicator_tema(data, period); }
}

class KAMA {
    constructor(period, fast, slow) {
        this._period = period;
        this._fastSc = 2.0 / ((fast || 2) + 1);
        this._slowSc = 2.0 / ((slow || 30) + 1);
        this._buffer = [];
        this._value = 0;
        this._count = 0;
    }
    update(value) {
        this._buffer.push(value);
        this._count++;
        if (this._count <= this._period) {
            this._value = value;
            return this._value;
        }
        if (this._buffer.length > this._period + 1) this._buffer.shift();
        var direction = Math.abs(value - this._buffer[0]);
        var volatility = 0;
        for (var i = 1; i < this._buffer.length; i++) {
            volatility += Math.abs(this._buffer[i] - this._buffer[i - 1]);
        }
        var er = volatility !== 0 ? direction / volatility : 0;
        var sc = er * (this._fastSc - this._slowSc) + this._slowSc;
        sc = sc * sc;
        this._value = this._value + sc * (value - this._value);
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._count > this._period; }
    reset() { this._buffer = []; this._value = 0; this._count = 0; }
    static compute(data, period, fast, slow) {
        return __flox_indicator_kama(data, period, fast || 2, slow || 30);
    }
}

// ============================================================
// Oscillators
// ============================================================

class RSI {
    constructor(period) {
        this._period = period;
        this._count = 0;
        this._prevValue = 0;
        this._avgGain = 0;
        this._avgLoss = 0;
        this._value = 50;
    }
    update(value) {
        if (this._count === 0) {
            this._prevValue = value;
            this._count++;
            return this._value;
        }
        var change = value - this._prevValue;
        this._prevValue = value;
        var gain = change > 0 ? change : 0;
        var loss = change < 0 ? -change : 0;
        if (this._count <= this._period) {
            this._avgGain += gain / this._period;
            this._avgLoss += loss / this._period;
            this._count++;
        } else {
            this._avgGain = (this._avgGain * (this._period - 1) + gain) / this._period;
            this._avgLoss = (this._avgLoss * (this._period - 1) + loss) / this._period;
        }
        if (this._avgLoss === 0) {
            this._value = 100;
        } else {
            this._value = 100 - 100 / (1 + this._avgGain / this._avgLoss);
        }
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._count > this._period; }
    reset() { this._count = 0; this._prevValue = 0; this._avgGain = 0; this._avgLoss = 0; this._value = 50; }
    static compute(data, period) { return __flox_indicator_rsi(data, period); }
}

class Stochastic {
    constructor(kPeriod, dPeriod) {
        this._kPeriod = kPeriod || 14;
        this._dPeriod = dPeriod || 3;
        this._highs = [];
        this._lows = [];
        this._dSma = new SMA(this._dPeriod);
        this._k = 0;
        this._d = 0;
    }
    update(high, low, close) {
        this._highs.push(high);
        this._lows.push(low);
        if (this._highs.length > this._kPeriod) {
            this._highs.shift();
            this._lows.shift();
        }
        if (this._highs.length >= this._kPeriod) {
            var hh = -Infinity, ll = Infinity;
            for (var i = 0; i < this._highs.length; i++) {
                if (this._highs[i] > hh) hh = this._highs[i];
                if (this._lows[i] < ll) ll = this._lows[i];
            }
            this._k = hh !== ll ? 100 * (close - ll) / (hh - ll) : 0;
            this._d = this._dSma.update(this._k);
        }
        return this._k;
    }
    get k() { return this._k; }
    get d() { return this._d; }
    get value() { return this._k; }
    get ready() { return this._highs.length >= this._kPeriod && this._dSma.ready; }
    reset() { this._highs = []; this._lows = []; this._dSma = new SMA(this._dPeriod); this._k = 0; this._d = 0; }
    static compute(high, low, close, kPeriod, dPeriod) {
        return __flox_indicator_stochastic(high, low, close, kPeriod || 14, dPeriod || 3);
    }
}

class CCI {
    constructor(period) {
        this._period = period || 20;
        this._buffer = [];
        this._value = 0;
    }
    update(high, low, close) {
        var tp = (high + low + close) / 3;
        this._buffer.push(tp);
        if (this._buffer.length > this._period) this._buffer.shift();
        if (this._buffer.length < this._period) return 0;
        var sum = 0;
        for (var i = 0; i < this._buffer.length; i++) sum += this._buffer[i];
        var mean = sum / this._buffer.length;
        var devSum = 0;
        for (var i = 0; i < this._buffer.length; i++) devSum += Math.abs(this._buffer[i] - mean);
        var meanDev = devSum / this._buffer.length;
        this._value = meanDev !== 0 ? (tp - mean) / (0.015 * meanDev) : 0;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._buffer.length >= this._period; }
    reset() { this._buffer = []; this._value = 0; }
    static compute(high, low, close, period) {
        return __flox_indicator_cci(high, low, close, period || 20);
    }
}

class CHOP {
    constructor(period) {
        this._period = period;
        this._atr1 = new ATR(1);
        this._atrSum = [];
        this._highs = [];
        this._lows = [];
        this._value = 0;
        this._log10Period = Math.log(period) / Math.LN10;
    }
    update(high, low, close) {
        var atrVal = this._atr1.update(high, low, close);
        this._atrSum.push(atrVal);
        this._highs.push(high);
        this._lows.push(low);
        if (this._atrSum.length > this._period) {
            this._atrSum.shift();
            this._highs.shift();
            this._lows.shift();
        }
        if (this._atrSum.length >= this._period) {
            var sum = 0;
            for (var i = 0; i < this._atrSum.length; i++) sum += this._atrSum[i];
            var hh = -Infinity, ll = Infinity;
            for (var i = 0; i < this._highs.length; i++) {
                if (this._highs[i] > hh) hh = this._highs[i];
                if (this._lows[i] < ll) ll = this._lows[i];
            }
            var range = hh - ll;
            this._value = range > 0 ? 100 * (Math.log(sum / range) / Math.LN10) / this._log10Period : 0;
        }
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._atrSum.length >= this._period; }
    reset() { this._atr1 = new ATR(1); this._atrSum = []; this._highs = []; this._lows = []; this._value = 0; }
    static compute(high, low, close, period) {
        return __flox_indicator_chop(high, low, close, period);
    }
}

// ============================================================
// Trend
// ============================================================

class ATR {
    constructor(period) {
        this._period = period;
        this._count = 0;
        this._prevClose = 0;
        this._value = 0;
        this._sum = 0;
    }
    update(high, low, close) {
        var tr;
        if (this._count === 0) {
            tr = high - low;
        } else {
            tr = Math.max(high - low, Math.abs(high - this._prevClose), Math.abs(low - this._prevClose));
        }
        this._prevClose = close;
        this._count++;
        if (this._count <= this._period) {
            this._sum += tr;
            this._value = this._sum / this._count;
        } else {
            this._value = (this._value * (this._period - 1) + tr) / this._period;
        }
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._count >= this._period; }
    reset() { this._count = 0; this._prevClose = 0; this._value = 0; this._sum = 0; }
    static compute(high, low, close, period) {
        return __flox_indicator_atr(high, low, close, period);
    }
}

class ADX {
    constructor(period) {
        this._period = period;
        this._count = 0;
        this._prevHigh = 0;
        this._prevLow = 0;
        this._prevClose = 0;
        this._smoothPlusDm = 0;
        this._smoothMinusDm = 0;
        this._smoothTr = 0;
        this._adxSum = 0;
        this._adxCount = 0;
        this._adx = 0;
        this._plusDi = 0;
        this._minusDi = 0;
    }
    update(high, low, close) {
        if (this._count === 0) {
            this._prevHigh = high;
            this._prevLow = low;
            this._prevClose = close;
            this._count++;
            return this._adx;
        }
        var tr = Math.max(high - low, Math.abs(high - this._prevClose), Math.abs(low - this._prevClose));
        var plusDm = high - this._prevHigh > this._prevLow - low && high - this._prevHigh > 0 ? high - this._prevHigh : 0;
        var minusDm = this._prevLow - low > high - this._prevHigh && this._prevLow - low > 0 ? this._prevLow - low : 0;
        this._prevHigh = high;
        this._prevLow = low;
        this._prevClose = close;
        this._count++;
        if (this._count <= this._period + 1) {
            this._smoothTr += tr;
            this._smoothPlusDm += plusDm;
            this._smoothMinusDm += minusDm;
        } else {
            this._smoothTr = this._smoothTr - this._smoothTr / this._period + tr;
            this._smoothPlusDm = this._smoothPlusDm - this._smoothPlusDm / this._period + plusDm;
            this._smoothMinusDm = this._smoothMinusDm - this._smoothMinusDm / this._period + minusDm;
        }
        if (this._count > this._period && this._smoothTr > 0) {
            this._plusDi = 100 * this._smoothPlusDm / this._smoothTr;
            this._minusDi = 100 * this._smoothMinusDm / this._smoothTr;
            var diSum = this._plusDi + this._minusDi;
            var dx = diSum > 0 ? 100 * Math.abs(this._plusDi - this._minusDi) / diSum : 0;
            if (this._adxCount < this._period) {
                this._adxSum += dx;
                this._adxCount++;
                this._adx = this._adxSum / this._adxCount;
            } else {
                this._adx = (this._adx * (this._period - 1) + dx) / this._period;
            }
        }
        return this._adx;
    }
    get adx() { return this._adx; }
    get plusDi() { return this._plusDi; }
    get minusDi() { return this._minusDi; }
    get value() { return this._adx; }
    get ready() { return this._count > 2 * this._period; }
    reset() { this._count = 0; this._prevHigh = 0; this._prevLow = 0; this._prevClose = 0; this._smoothPlusDm = 0; this._smoothMinusDm = 0; this._smoothTr = 0; this._adxSum = 0; this._adxCount = 0; this._adx = 0; this._plusDi = 0; this._minusDi = 0; }
    static compute(high, low, close, period) {
        return __flox_indicator_adx(high, low, close, period);
    }
}

class Slope {
    constructor(length) {
        this._length = length;
        this._buffer = [];
        this._value = 0;
    }
    update(value) {
        this._buffer.push(value);
        if (this._buffer.length > this._length + 1) this._buffer.shift();
        if (this._buffer.length > this._length) {
            this._value = (value - this._buffer[0]) / this._length;
        }
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._buffer.length > this._length; }
    reset() { this._buffer = []; this._value = 0; }
    static compute(data, length) { return __flox_indicator_slope(data, length); }
}

class MACD {
    constructor(fastPeriod, slowPeriod, signalPeriod) {
        this._fastEma = new EMA(fastPeriod || 12);
        this._slowEma = new EMA(slowPeriod || 26);
        this._signalEma = new EMA(signalPeriod || 9);
        this._line = 0;
        this._signal = 0;
        this._histogram = 0;
        this._ready = false;
    }
    update(value) {
        var fast = this._fastEma.update(value);
        var slow = this._slowEma.update(value);
        this._line = fast - slow;
        if (this._slowEma.ready) {
            this._signal = this._signalEma.update(this._line);
            this._ready = this._signalEma.ready;
        }
        this._histogram = this._line - this._signal;
        return this._line;
    }
    get line() { return this._line; }
    get signal() { return this._signal; }
    get histogram() { return this._histogram; }
    get value() { return this._line; }
    get ready() { return this._ready; }
    reset() { this._fastEma.reset(); this._slowEma.reset(); this._signalEma.reset(); this._line = 0; this._signal = 0; this._histogram = 0; this._ready = false; }
    static compute(data, fast, slow, signal) {
        return __flox_indicator_macd(data, fast || 12, slow || 26, signal || 9);
    }
}

class Bollinger {
    constructor(period, multiplier) {
        this._period = period || 20;
        this._multiplier = multiplier || 2.0;
        this._sma = new SMA(this._period);
        this._buffer = [];
        this._upper = 0;
        this._middle = 0;
        this._lower = 0;
    }
    update(value) {
        this._middle = this._sma.update(value);
        this._buffer.push(value);
        if (this._buffer.length > this._period) this._buffer.shift();
        if (this._sma.ready) {
            var sum = 0;
            for (var i = 0; i < this._buffer.length; i++) {
                var diff = this._buffer[i] - this._middle;
                sum += diff * diff;
            }
            var std = Math.sqrt(sum / this._buffer.length);
            this._upper = this._middle + this._multiplier * std;
            this._lower = this._middle - this._multiplier * std;
        }
        return this._middle;
    }
    get upper() { return this._upper; }
    get middle() { return this._middle; }
    get lower() { return this._lower; }
    get value() { return this._middle; }
    get ready() { return this._sma.ready; }
    reset() { this._sma = new SMA(this._period); this._buffer = []; this._upper = 0; this._middle = 0; this._lower = 0; }
    static compute(data, period, multiplier) {
        return __flox_indicator_bollinger(data, period || 20, multiplier || 2.0);
    }
}

// ============================================================
// Volume
// ============================================================

class OBV {
    constructor() {
        this._value = 0;
        this._prevClose = 0;
        this._count = 0;
    }
    update(close, volume) {
        if (this._count === 0) {
            this._value = volume;
        } else if (close > this._prevClose) {
            this._value += volume;
        } else if (close < this._prevClose) {
            this._value -= volume;
        }
        this._prevClose = close;
        this._count++;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._count > 0; }
    reset() { this._value = 0; this._prevClose = 0; this._count = 0; }
    static compute(close, volume) { return __flox_indicator_obv(close, volume); }
}

class VWAP {
    constructor(window) {
        this._window = window;
        this._prices = [];
        this._volumes = [];
        this._value = 0;
    }
    update(close, volume) {
        this._prices.push(close);
        this._volumes.push(volume);
        if (this._prices.length > this._window) {
            this._prices.shift();
            this._volumes.shift();
        }
        var pvSum = 0, vSum = 0;
        for (var i = 0; i < this._prices.length; i++) {
            pvSum += this._prices[i] * this._volumes[i];
            vSum += this._volumes[i];
        }
        this._value = vSum > 0 ? pvSum / vSum : 0;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._prices.length >= this._window; }
    reset() { this._prices = []; this._volumes = []; this._value = 0; }
    static compute(close, volume, window) {
        return __flox_indicator_vwap(close, volume, window);
    }
}

class CVD {
    constructor() {
        this._value = 0;
        this._count = 0;
    }
    update(open, high, low, close, volume) {
        var range = high - low;
        var delta = range > 0 ? volume * (close - open) / range : 0;
        this._value += delta;
        this._count++;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._count > 0; }
    reset() { this._value = 0; this._count = 0; }
    static compute(open, high, low, close, volume) {
        return __flox_indicator_cvd(open, high, low, close, volume);
    }
}

// ============================================================
// Statistical
// ============================================================

class Skewness {
    constructor(period) {
        this._period = period;
        this._buffer = [];
        this._value = NaN;
    }
    update(value) {
        this._buffer.push(value);
        if (this._buffer.length > this._period) this._buffer.shift();
        if (this._buffer.length < this._period) return this._value;
        var n = this._buffer.length;
        var sum = 0;
        for (var i = 0; i < n; i++) sum += this._buffer[i];
        var mean = sum / n;
        var s2 = 0;
        for (var i = 0; i < n; i++) { var d = this._buffer[i] - mean; s2 += d * d; }
        var s = Math.sqrt(s2 / (n - 1));
        if (s === 0) { this._value = NaN; return this._value; }
        var m3 = 0;
        for (var i = 0; i < n; i++) { var d = (this._buffer[i] - mean) / s; m3 += d * d * d; }
        this._value = (n / ((n - 1) * (n - 2))) * m3;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._buffer.length >= this._period; }
    reset() { this._buffer = []; this._value = NaN; }
    static compute(data, period) { return __flox_indicator_skewness(data, period); }
}

class Kurtosis {
    constructor(period) {
        this._period = period;
        this._buffer = [];
        this._value = NaN;
    }
    update(value) {
        this._buffer.push(value);
        if (this._buffer.length > this._period) this._buffer.shift();
        if (this._buffer.length < this._period) return this._value;
        var n = this._buffer.length;
        var sum = 0;
        for (var i = 0; i < n; i++) sum += this._buffer[i];
        var mean = sum / n;
        var s2 = 0;
        for (var i = 0; i < n; i++) { var d = this._buffer[i] - mean; s2 += d * d; }
        var s = Math.sqrt(s2 / (n - 1));
        if (s === 0) { this._value = NaN; return this._value; }
        var m4 = 0;
        for (var i = 0; i < n; i++) { var d = (this._buffer[i] - mean) / s; m4 += d * d * d * d; }
        this._value = (n * (n + 1)) / ((n - 1) * (n - 2) * (n - 3)) * m4 - 3 * (n - 1) * (n - 1) / ((n - 2) * (n - 3));
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._buffer.length >= this._period; }
    reset() { this._buffer = []; this._value = NaN; }
    static compute(data, period) { return __flox_indicator_kurtosis(data, period); }
}

class RollingZScore {
    constructor(period) {
        this._period = period;
        this._buffer = [];
        this._value = NaN;
    }
    update(value) {
        this._buffer.push(value);
        if (this._buffer.length > this._period) this._buffer.shift();
        if (this._buffer.length < this._period) return this._value;
        var n = this._buffer.length;
        var sum = 0;
        for (var i = 0; i < n; i++) sum += this._buffer[i];
        var mean = sum / n;
        var s2 = 0;
        for (var i = 0; i < n; i++) { var d = this._buffer[i] - mean; s2 += d * d; }
        var s = Math.sqrt(s2 / n);
        this._value = s === 0 ? NaN : (value - mean) / s;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._buffer.length >= this._period; }
    reset() { this._buffer = []; this._value = NaN; }
    static compute(data, period) { return __flox_indicator_rolling_zscore(data, period); }
}

class ShannonEntropy {
    constructor(period, bins) {
        this._period = period;
        this._bins = bins || 10;
        this._buffer = [];
        this._value = NaN;
    }
    update(value) {
        this._buffer.push(value);
        if (this._buffer.length > this._period) this._buffer.shift();
        if (this._buffer.length < this._period) return this._value;
        var n = this._buffer.length;
        var min = Infinity, max = -Infinity;
        for (var i = 0; i < n; i++) {
            if (this._buffer[i] < min) min = this._buffer[i];
            if (this._buffer[i] > max) max = this._buffer[i];
        }
        if (max === min) { this._value = 0; return this._value; }
        var counts = new Array(this._bins);
        for (var i = 0; i < this._bins; i++) counts[i] = 0;
        var range = max - min;
        for (var i = 0; i < n; i++) {
            var bin = Math.floor((this._buffer[i] - min) / range * this._bins);
            if (bin >= this._bins) bin = this._bins - 1;
            counts[bin]++;
        }
        var entropy = 0;
        var lnBins = Math.log(this._bins);
        for (var i = 0; i < this._bins; i++) {
            if (counts[i] > 0) {
                var p = counts[i] / n;
                entropy -= p * Math.log(p);
            }
        }
        this._value = lnBins > 0 ? entropy / lnBins : 0;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._buffer.length >= this._period; }
    reset() { this._buffer = []; this._value = NaN; }
    static compute(data, period, bins) { return __flox_indicator_shannon_entropy(data, period, bins || 10); }
}

class ParkinsonVol {
    constructor(period) {
        this._period = period;
        this._buffer = [];
        this._value = NaN;
    }
    update(high, low) {
        var hl = Math.log(high / low);
        this._buffer.push(hl * hl);
        if (this._buffer.length > this._period) this._buffer.shift();
        if (this._buffer.length < this._period) return this._value;
        var sum = 0;
        for (var i = 0; i < this._buffer.length; i++) sum += this._buffer[i];
        this._value = Math.sqrt(sum / this._buffer.length / (4 * Math.LN2));
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._buffer.length >= this._period; }
    reset() { this._buffer = []; this._value = NaN; }
    static compute(high, low, period) { return __flox_indicator_parkinson_vol(high, low, period); }
}

class RogersSatchellVol {
    constructor(period) {
        this._period = period;
        this._buffer = [];
        this._value = NaN;
    }
    update(open, high, low, close) {
        var rs = Math.log(high / close) * Math.log(high / open) + Math.log(low / close) * Math.log(low / open);
        this._buffer.push(rs);
        if (this._buffer.length > this._period) this._buffer.shift();
        if (this._buffer.length < this._period) return this._value;
        var sum = 0;
        for (var i = 0; i < this._buffer.length; i++) sum += this._buffer[i];
        this._value = Math.sqrt(sum / this._buffer.length);
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._buffer.length >= this._period; }
    reset() { this._buffer = []; this._value = NaN; }
    static compute(open, high, low, close, period) { return __flox_indicator_rogers_satchell_vol(open, high, low, close, period); }
}

class Correlation {
    constructor(period) {
        this._period = period;
        this._xs = [];
        this._ys = [];
        this._value = NaN;
    }
    update(x, y) {
        this._xs.push(x);
        this._ys.push(y);
        if (this._xs.length > this._period) { this._xs.shift(); this._ys.shift(); }
        if (this._xs.length < this._period) return this._value;
        var n = this._xs.length;
        var sx = 0, sy = 0;
        for (var i = 0; i < n; i++) { sx += this._xs[i]; sy += this._ys[i]; }
        var mx = sx / n, my = sy / n;
        var sxy = 0, sxx = 0, syy = 0;
        for (var i = 0; i < n; i++) {
            var dx = this._xs[i] - mx, dy = this._ys[i] - my;
            sxy += dx * dy;
            sxx += dx * dx;
            syy += dy * dy;
        }
        var denom = Math.sqrt(sxx * syy);
        this._value = denom === 0 ? NaN : sxy / denom;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._xs.length >= this._period; }
    reset() { this._xs = []; this._ys = []; this._value = NaN; }
    static compute(x, y, period) { return __flox_indicator_correlation(x, y, period); }
}

class AutoCorrelation {
    constructor(window, lag) {
        this._window = window;
        this._lag = lag;
        this._buf = [];
        this._value = NaN;
    }
    update(x) {
        this._buf.push(x);
        var needed = this._window + this._lag;
        if (this._buf.length > needed) this._buf.shift();
        if (this._buf.length < needed) return this._value;
        var w = this._window;
        var sx = 0, sy = 0, sxy = 0, sx2 = 0, sy2 = 0;
        for (var i = 0; i < w; ++i) {
            var xi = this._buf[i + this._lag];
            var yi = this._buf[i];
            sx += xi; sy += yi;
            sxy += xi * yi;
            sx2 += xi * xi;
            sy2 += yi * yi;
        }
        var num = w * sxy - sx * sy;
        var den = Math.sqrt((w * sx2 - sx * sx) * (w * sy2 - sy * sy));
        this._value = den === 0 ? NaN : num / den;
        return this._value;
    }
    get value() { return this._value; }
    get ready() { return this._buf.length >= this._window + this._lag; }
    reset() { this._buf = []; this._value = NaN; }
    static compute(data, window, lag) { return __flox_indicator_autocorrelation(data, window, lag); }
}
