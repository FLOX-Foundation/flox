// Multi-feed clock for QuickJS strategies. Mirrors the
// NAPI / pybind11 / Codon surfaces. Pure JS facade over the C ABI
// flox_feed_clock_* exports — the engine state lives in C++.

var FeedClockPolicy = Object.freeze({
    WaitForAll: 'WaitForAll',
    FireOnAny: 'FireOnAny',
    LeaderFollower: 'LeaderFollower',
});

var _FEED_POLICY_TO_INT = { 'WaitForAll': 0, 'FireOnAny': 1, 'LeaderFollower': 2 };

function _resolveFeedPolicy(p) {
    if (typeof p === 'string') {
        var v = _FEED_POLICY_TO_INT[p];
        if (v === undefined) {
            throw new Error("Unknown FeedClock policy: '" + p + "'. Use 'WaitForAll' / 'FireOnAny' / 'LeaderFollower'.");
        }
        return v;
    }
    if (typeof p === 'number') return p;
    return 0;
}

class MultiFeedClock {
    constructor(opts) {
        if (!opts || !Array.isArray(opts.symbols)) {
            throw new Error("MultiFeedClock requires { symbols: number[] }");
        }
        var policy = _resolveFeedPolicy(opts.policy);
        var timeout = opts.timeoutMs === undefined ? 200 : opts.timeoutMs;
        var leader = opts.leaderSymbol === undefined ? 0 : opts.leaderSymbol;
        var budget = opts.stalenessBudgetMs === undefined ? 200 : opts.stalenessBudgetMs;
        // The handle is a numeric pointer (cobj) returned through the C ABI;
        // we store it untouched and pass it back to every method.
        // __flox_feed_clock_create takes (symbols_array, count, policy, timeoutMs, leader, budget).
        this._h = __flox_feed_clock_create(opts.symbols, opts.symbols.length,
            policy, timeout, leader, budget);
        this._symbols = opts.symbols.slice();
    }

    symbolCount() { return __flox_feed_clock_symbol_count(this._h); }

    tick(tsNs, symbol) {
        var fired = __flox_feed_clock_tick(this._h, tsNs, symbol) !== 0;
        var n = __flox_feed_clock_symbol_count(this._h);
        var lastTsNs = {};
        var stalenessNs = {};
        for (var i = 0; i < n; i++) {
            var s = __flox_feed_clock_symbol_at(this._h, i);
            lastTsNs[s] = __flox_feed_clock_last_seen_at(this._h, i);
            stalenessNs[s] = __flox_feed_clock_staleness_at(this._h, i);
        }
        return {
            fired: fired,
            triggeredBy: __flox_feed_clock_last_triggered_by(this._h),
            lastTsNs: lastTsNs,
            stalenessNs: stalenessNs,
        };
    }

    reset() { __flox_feed_clock_reset(this._h); }
}
