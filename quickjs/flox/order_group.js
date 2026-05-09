// Multi-leg order group state machine for QuickJS strategies.
// Mirrors the NAPI / pybind11 / Codon surfaces — passive state machine
// that records submit / fill / cancel events and reports the group
// state + recommended actions. The strategy is responsible for wiring
// actions into the executor.
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
        this._legs = [];
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
        var idx = this._legs.length;
        this._legs.push({
            symbol: symbol, side: side, targetQty: qty,
            orderType: 1, limitPrice: 0,
            orderId: 0, filledQty: 0, state: 0,
        });
        return idx;
    }

    addLimitLeg(symbol, side, price, qty) {
        var idx = this._legs.length;
        this._legs.push({
            symbol: symbol, side: side, targetQty: qty,
            orderType: 0, limitPrice: price,
            orderId: 0, filledQty: 0, state: 0,
        });
        return idx;
    }

    legCount() { return this._legs.length; }
    legState(i) { return _LEG_STATE_NAMES[this._legs[i].state]; }
    legFilled(i) { return this._legs[i].filledQty; }
    legOrderId(i) { return this._legs[i].orderId; }

    recordSubmit(i, orderId) {
        this._legs[i].orderId = orderId;
        this._legs[i].state = 1;
    }

    recordFill(i, cumulativeQty) {
        var leg = this._legs[i];
        leg.filledQty = cumulativeQty;
        leg.state = (cumulativeQty >= leg.targetQty) ? 3 : 2;
    }

    recordCancel(i) { this._legs[i].state = 4; }
    recordFailure(i) { this._legs[i].state = 5; }

    markActionDispatched(legIndex, kind) {
        var bit = (kind === 'cancel') ? 0x1 : 0x2;
        this._legs[legIndex].dispatched = (this._legs[legIndex].dispatched || 0) | bit;
    }

    setPairLatencyBudgetNs(budgetNs) {
        this._pairBudgetNs = budgetNs;
    }

    pairLatencyDecision(opts) {
        opts = opts || {};
        var budget = this._pairBudgetNs || 0;
        if (budget <= 0) return 'wait';
        var submit = opts.leaderSubmitTsNs || 0;
        var ack = opts.leaderAckTsNs || 0;
        var ackReceived = opts.ackReceived === true;
        if (ackReceived) {
            return (ack - submit <= budget) ? 'submit_follower' : 'cancel_leader';
        }
        // No ack yet — caller passed current feed-time as ackTs.
        if (ack - submit > budget) return 'cancel_leader';
        return 'wait';
    }

    setRiskLimits(opts) {
        opts = opts || {};
        this._limits = {
            maxGrossNotional: opts.maxGrossNotional || 0,
            maxConcentrationPct: opts.maxConcentrationPct || 0,
            maxLegQty: opts.maxLegQty || 0,
        };
    }

    precheckSubmission(opts) {
        opts = opts || {};
        var limits = this._limits || { maxGrossNotional: 0, maxConcentrationPct: 0, maxLegQty: 0 };
        if (limits.maxGrossNotional === 0 && limits.maxConcentrationPct === 0
            && limits.maxLegQty === 0) {
            return { denied: false, rule: '', detail: '' };
        }
        var equity = opts.equity || 0;
        var refPrices = opts.marketRefPrices || [];
        var grossNotional = 0;
        for (var i = 0; i < this._legs.length; i++) {
            var l = this._legs[i];
            if (limits.maxLegQty !== 0 && l.targetQty > limits.maxLegQty) {
                return { denied: true, rule: 'maxLegQty',
                         detail: 'leg ' + i + ' qty exceeds per-leg cap' };
            }
            var price = 0;
            if (l.orderType === 0) price = l.limitPrice;
            else if (i < refPrices.length) price = refPrices[i];
            grossNotional += Math.abs(price * l.targetQty);
        }
        if (limits.maxGrossNotional !== 0 && grossNotional > limits.maxGrossNotional) {
            return { denied: true, rule: 'maxGrossNotional',
                     detail: 'basket gross notional exceeds cap' };
        }
        if (limits.maxConcentrationPct > 0 && equity > 0) {
            var frac = grossNotional / equity;
            if (frac > limits.maxConcentrationPct) {
                return { denied: true, rule: 'maxConcentrationPct',
                         detail: 'basket gross notional exceeds concentration limit vs equity' };
            }
        }
        return { denied: false, rule: '', detail: '' };
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

    _stateRaw() {
        if (this._legs.length === 0) return 0;
        var anyPending = false, anyFilled = false, allFilled = true;
        var anyFailed = false, anyCancelled = false, allTerminal = true;
        var anyNonFilledTerminal = false;
        for (var i = 0; i < this._legs.length; i++) {
            var s = this._legs[i].state;
            switch (s) {
                case 0: anyPending = true; allTerminal = false; allFilled = false; break;
                case 1:
                case 2: allTerminal = false; allFilled = false; break;
                case 3: anyFilled = true; break;
                case 4: anyCancelled = true; allFilled = false; anyNonFilledTerminal = true; break;
                case 5: anyFailed = true; allFilled = false; anyNonFilledTerminal = true; break;
            }
        }
        if (anyPending) return 0;
        if (allFilled) return 3;
        if (this._policy === 1 && anyFailed) return 5;
        if (this._policy === 2 && anyFilled && !allFilled) return 2;
        if (allTerminal && !anyFilled) {
            return anyCancelled ? 4 : 6;
        }
        if (anyFilled || anyNonFilledTerminal) return 2;
        return 1;
    }

    state() { return _GROUP_STATE_NAMES[this._stateRaw()]; }

    recommendedActions() {
        var out = [];
        if (this._policy === 0) return out;
        if (this._policy === 2) {
            var anyFill = false;
            for (var i = 0; i < this._legs.length; i++) {
                if (this._legs[i].state === 3 || this._legs[i].state === 2) { anyFill = true; break; }
            }
            if (!anyFill) return out;
            for (var j = 0; j < this._legs.length; j++) {
                var l = this._legs[j];
                var dispatched = l.dispatched || 0;
                if ((l.state === 0 || l.state === 1) && !(dispatched & 0x1)) {
                    out.push({ kind: 'cancel', legIndex: j, orderId: l.orderId });
                }
            }
            return out;
        }
        if (this._policy === 1) {
            var anyFailureOrCancel = false;
            for (var k = 0; k < this._legs.length; k++) {
                var st = this._legs[k].state;
                if (st === 5 || st === 4) { anyFailureOrCancel = true; break; }
            }
            if (!anyFailureOrCancel) return out;
            for (var m = 0; m < this._legs.length; m++) {
                var ll = this._legs[m];
                var disp = ll.dispatched || 0;
                if ((ll.state === 0 || ll.state === 1) && !(disp & 0x1)) {
                    out.push({ kind: 'cancel', legIndex: m, orderId: ll.orderId });
                } else if ((ll.state === 3 || ll.state === 2) && !(disp & 0x2)) {
                    out.push({
                        kind: 'revert',
                        legIndex: m,
                        symbol: ll.symbol,
                        side: ll.side === 0 ? 1 : 0,
                        qty: ll.filledQty,
                    });
                }
            }
            return out;
        }
        return out;
    }
}
