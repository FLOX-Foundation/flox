# Model venue rate limits in the simulator

Real exchanges throttle: weighted per-endpoint quotas, per-account
order-action caps, burst bans after sustained 429s. Without
modelling this, a market-maker backtest that puts up 800 orders/day
looks identical to one that puts up 8000 — even though the real
account would have spent the second half of the day rate-limited.

`RateLimitPolicy` adds a sliding-window quota model to
SimulatedExecutor. Each submit / cancel / replace consults the
policy first; an overflow emits `REJECTED_RATE_LIMIT` and the
action is not committed to the simulator.

## What you configure

- One or more **buckets**, each with a window length, a capacity,
  and per-action weights. The standard pattern is two buckets — a
  short burst window (10s) and a sustained one (60s).
- An optional **ban rule**: after N consecutive rejects, ban every
  action for a fixed duration. Models the 3-minute IP ban that
  some venues apply after sustained 429s.

## Canned profiles

| Profile               | Buckets                      | Ban             |
|-----------------------|------------------------------|-----------------|
| `binance_um_futures`  | 50 / 10s, 300 / 60s          | 3 rejects → 3m  |
| `bybit_linear`        | 10 / 1s, 100 / 60s           | 5 rejects → 1m  |
| `okx_swap`            | 60 / 2s                      | 3 rejects → 2m  |
| `deribit`             | 5 / 0.5s, 60 / 60s           | 3 rejects → 1m  |

Numbers approximate published rules; tune to your account tier.

## Apply from a strategy

=== "Python"

    ```python
    --8<-- "examples/python_rate_limit_policy.py"
    ```

=== "Node.js"

    ```javascript
    const flox = require('flox-node');
    const policy = new flox.RateLimitPolicy();
    policy.loadProfile('binance_um_futures');
    exec.setRateLimitPolicy(policy);
    ```

=== "Codon"

    ```python
    from flox.rate_limit import RateLimitPolicy

    p = RateLimitPolicy()
    p.load_profile("binance_um_futures")
    exec.set_rate_limit_policy(p)
    ```

=== "QuickJS"

    ```javascript
    const p = __flox_rate_limit_policy_create();
    __flox_rate_limit_policy_load_profile(p, "binance_um_futures");
    __flox_simulated_executor_set_rate_limit_policy(exec, p);
    ```

=== "C++"

    ```cpp
    #include "flox/backtest/rate_limit_policy.h"
    auto p = flox::RateLimitPolicy::binance_um_futures();
    sim.setRateLimitPolicy(p);
    ```

## Reading remaining capacity

`bucket_states(now_ns)` returns per-bucket usage. Use this to
implement back-off in the strategy before the venue rejects you:

```python
for s in policy.bucket_states(now_ns):
    headroom = s["capacity"] - s["used"]
    if headroom < 5:
        # back off — only a few slots left in this window
        pass
```

## Notes

- Reject-check is atomic across buckets: if any single bucket would
  overflow, none of them get charged. Stays consistent if you have
  three buckets configured and only the third one rejects.
- The replace action defaults to weight 2 across all canned
  profiles, matching most venues' published rules.
- Once a ban fires, every subsequent action rejects (no matter the
  per-bucket headroom) until the ban window expires.
- Set `policy.set_ban(0, 0)` to disable the ban mechanism.
