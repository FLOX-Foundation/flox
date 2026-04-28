// Targets — forward-looking labels (research-only).
//
// Targets read into the future relative to t. They live under `flox.targets.*`
// and are intentionally batch-only: feeding them into a live update loop is
// a look-ahead-bias bug.

var __floxTargets = {
    future_return: function(close, horizon) {
        return __flox_target_future_return(close, horizon);
    },
    future_ctc_volatility: function(close, horizon) {
        return __flox_target_future_ctc_volatility(close, horizon);
    },
    future_linear_slope: function(close, horizon) {
        return __flox_target_future_linear_slope(close, horizon);
    }
};
