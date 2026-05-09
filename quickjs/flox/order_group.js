// Multi-leg order group state machine for QuickJS strategies.
// Thin wrapper over the C ABI flox_order_group_* exports — same as
// the pybind11 / NAPI / Codon bindings. The state machine lives in
// the C++ engine; this class is a shim, not a reimplementation.
//
// Policy + state are exposed as string-named constants (BestEffort /
// AllOrNothing / OneSided; Pending / Submitted / Filled / ...).
// Numeric values are an implementation detail — never hand-write them.

var OrderGroupPolicy = Object.freeze({
    BestEffort: 'BestEffort',
    AllOrNothing: 'AllOrNothing',
    OneSided: 'OneSided',
});

var OrderGroupState = Object.freeze({
    Pending: 'Pending',
    Submitted: 'Submitted',
    PartiallyFilled: 'PartiallyFilled',
    Filled: 'Filled',
    Cancelled: 'Cancelled',
    Reverting: 'Reverting',
    Failed: 'Failed',
});

var LegState = Object.freeze({
    Pending: 'Pending',
    Submitted: 'Submitted',
    PartiallyFilled: 'PartiallyFilled',
    Filled: 'Filled',
    Cancelled: 'Cancelled',
    Failed: 'Failed',
});

var _POLICY_TO_INT = { 'BestEffort': 0, 'AllOrNothing': 1, 'OneSided': 2 };
var _GROUP_STATE_NAMES = [
    'Pending', 'Submitted', 'PartiallyFilled', 'Filled',
    'Cancelled', 'Reverting', 'Failed',
];
var _LEG_STATE_NAMES = [
    'Pending', 'Submitted', 'PartiallyFilled', 'Filled', 'Cancelled', 'Failed',
];

function _resolvePolicy(p) {
    if (typeof p === 'string') {
        var v = _POLICY_TO_INT[p];
        if (v === undefined) throw new Error("Unknown OrderGroup policy: '" + p + "'. Use 'BestEffort' / 'AllOrNothing' / 'OneSided'.");
        return v;
    }
    if (typeof p === 'number') return p;
    return 0;
}

class OrderGroup {
    constructor(opts) {
        opts = opts || {};
        this._parent = opts.parentSignalId || 0;
        this._policy = _resolvePolicy(opts.policy);
        this._h = __flox_order_group_create(this._parent, this._policy);
    }

    destroy() {
        if (this._h) { __flox_order_group_destroy(this._h); this._h = null; }
    }

    parentSignalId() { return this._parent; }
    policy() {
        var keys = Object.keys(_POLICY_TO_INT);
        for (var i = 0; i < keys.length; i++) {
            if (_POLICY_TO_INT[keys[i]] === this._policy) return keys[i];
        }
        return 'BestEffort';
    }

    addMarketLeg(symbol, side, qty) {
        return __flox_order_group_add_market_leg(this._h, symbol, side, qty);
    }

    addLimitLeg(symbol, side, price, qty) {
        return __flox_order_group_add_limit_leg(this._h, symbol, side, price, qty);
    }

    legCount() { return __flox_order_group_leg_count(this._h); }
    legState(i) { return _LEG_STATE_NAMES[__flox_order_group_leg_state(this._h, i)]; }
    legFilled(i) { return __flox_order_group_leg_filled(this._h, i); }
    legOrderId(i) { return __flox_order_group_leg_order_id(this._h, i); }

    recordSubmit(i, orderId) {
        __flox_order_group_record_submit(this._h, i, orderId);
    }

    recordFill(i, cumulativeQty) {
        __flox_order_group_record_fill(this._h, i, cumulativeQty);
    }

    recordCancel(i) { __flox_order_group_record_cancel(this._h, i); }
    recordFailure(i) { __flox_order_group_record_failure(this._h, i); }

    markActionDispatched(legIndex, kind) {
        var k = (kind === 'cancel') ? 0 : 1;
        __flox_order_group_mark_action_dispatched(this._h, legIndex, k);
    }

    setPairLatencyBudgetNs(budgetNs) {
        __flox_order_group_set_pair_latency_budget_ns(this._h, budgetNs);
    }

    pairLatencyDecision(opts) {
        opts = opts || {};
        return __flox_order_group_pair_latency_decision(
            this._h,
            opts.leaderSubmitTsNs || 0,
            opts.leaderAckTsNs || 0,
            opts.ackReceived === true ? 1 : 0);
    }

    setRiskLimits(opts) {
        opts = opts || {};
        __flox_order_group_set_risk_limits(
            this._h,
            opts.maxGrossNotional || 0,
            opts.maxConcentrationPct || 0,
            opts.maxLegQty || 0);
    }

    precheckSubmission(opts) {
        opts = opts || {};
        return __flox_order_group_precheck_submission(
            this._h,
            opts.equity || 0,
            opts.marketRefPrices || []);
    }

    autoDispatch(strategy) {
        // Dispatch every not-yet-dispatched recommended action through
        // the strategy's emit helpers; mark each so it doesn't fire
        // again on subsequent calls.
        var actions = this.recommendedActions();
        var fired = 0;
        for (var i = 0; i < actions.length; i++) {
            var a = actions[i];
            if (a.kind === 'cancel') {
                strategy.cancel(a.orderId);
            } else {
                if (a.side === 0) {
                    strategy.marketBuy({ symbol: a.symbol, qty: a.qty });
                } else {
                    strategy.marketSell({ symbol: a.symbol, qty: a.qty });
                }
            }
            this.markActionDispatched(a.legIndex, a.kind);
            fired++;
        }
        return fired;
    }

    state() { return _GROUP_STATE_NAMES[__flox_order_group_state(this._h)]; }

    recommendedActions() {
        return __flox_order_group_recommended_actions(this._h);
    }
}
