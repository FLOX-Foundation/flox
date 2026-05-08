// quickjs/flox/latency.js
//
// Latency models for backtest realism. Each class wraps a
// FloxLatencyModelHandle through the QuickJS bridge and exposes the
// same surface as the Python and Node bindings: feedDelay /
// orderDelay / fillDelay / sample / reset.

class _LatencyBase {
    constructor(handle) { this._h = handle; }
    feedDelay() { return __flox_lat_feed_delay(this._h); }
    orderDelay() { return __flox_lat_order_delay(this._h); }
    fillDelay() { return __flox_lat_fill_delay(this._h); }
    sample() { return __flox_lat_sample(this._h); }
    reset(seed) { __flox_lat_reset(this._h, (seed === undefined) ? 0 : seed); }
    destroy() {
        if (this._h) { __flox_lat_destroy(this._h); this._h = null; }
    }
}

class ConstantLatency extends _LatencyBase {
    constructor(opts) {
        opts = opts || {};
        const h = __flox_lat_constant_create(
            opts.feedNs || 0, opts.orderNs || 0, opts.fillNs || 0);
        super(h);
    }
}

class GaussianLatency extends _LatencyBase {
    constructor(opts) {
        opts = opts || {};
        const h = __flox_lat_gaussian_create(
            opts.feedMeanNs || 0, opts.feedStddevNs || 0,
            opts.orderMeanNs || 0, opts.orderStddevNs || 0,
            opts.fillMeanNs || 0, opts.fillStddevNs || 0,
            opts.seed || 0);
        super(h);
    }
}

class ExponentialLatency extends _LatencyBase {
    constructor(opts) {
        opts = opts || {};
        const h = __flox_lat_exponential_create(
            opts.feedMeanNs || 0, opts.orderMeanNs || 0,
            opts.fillMeanNs || 0, opts.seed || 0);
        super(h);
    }
}

class EmpiricalLatency extends _LatencyBase {
    constructor(opts) {
        opts = opts || {};
        const h = __flox_lat_empirical_create(
            opts.feedSamples || [],
            opts.orderSamples || [],
            opts.fillSamples || [],
            opts.seed || 0);
        super(h);
    }
}
