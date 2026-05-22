# Configure realistic ack-latency distributions

The default ack-latency knobs (`set_submit_ack_latency(latency_ns,
jitter_ns)`) sample uniform jitter around a fixed base. Real venue
latency is heavy-tailed (lognormal-like p50 ≈ 5ms, p99 ≈ 80ms) and
*correlated*: when the venue is under load, every order is slow,
not each one independently.

`LatencyDistribution` ships four kinds plus a burst-correlation
knob. The scalar `set_submit_ack_latency` setter still works and
maps to the Uniform kind for backward compatibility.

## Kinds

| Kind         | Use when                                                 |
|--------------|----------------------------------------------------------|
| `Constant`   | Deterministic baselines, parity tests.                   |
| `Uniform`    | Legacy `base ± jitter` behaviour.                        |
| `Lognormal`  | Real venue ack timings — heavy right tail.               |
| `Empirical`  | You have a recorded histogram and want to resample it.   |

## Burst correlation

Set with `set_burst_correlation(rho)` where `rho` is in `[0, 1)`.
Implementation is AR(1) on the standard-normal residual for
Lognormal (or on the rank index for Empirical). The independent-
draw default is `rho = 0`.

Why care: a strategy that places, races, and cancels every 50ms
under a uniform model behaves very differently from the same
strategy under a `rho = 0.6` burst regime, where slow acks cluster.
The latter matches what live traders observe during venue load
spikes.

## Apply from a strategy

=== "Python"

    ```python
    --8<-- "examples/python_latency_distribution.py"
    ```

=== "Node.js"

    ```javascript
    const flox = require('flox-node');
    const dist = new flox.LatencyDistribution();
    dist.setLognormal(5_000_000, 0.7);
    dist.setBurstCorrelation(0.4);
    exec.setSubmitAckLatencyDistribution(dist);
    ```

=== "Codon"

    ```python
    from flox.backtest import SimulatedExecutor, LatencyDistribution

    dist = LatencyDistribution.lognormal(5_000_000, 0.7)
    dist.set_burst_correlation(0.4)
    exec = SimulatedExecutor()
    exec.set_submit_ack_latency_distribution(dist)
    ```

=== "QuickJS"

    ```javascript
    const dist = __flox_latency_distribution_create();
    __flox_latency_distribution_set_lognormal(dist, 5000000n, 0.7);
    __flox_latency_distribution_set_burst_correlation(dist, 0.4);
    __flox_simulated_executor_set_submit_ack_latency_distribution(exec, dist);
    ```

=== "C++"

    ```cpp
    #include "flox/backtest/latency_distribution.h"
    auto dist = flox::LatencyDistribution::lognormal(5'000'000, 0.7);
    dist.setBurstCorrelation(0.4);
    sim.setSubmitAckLatencyDistribution(dist);
    ```

## Calibrating from real data

A first-pass workflow when you have a venue tape with timestamped
submits and acks:

1. Compute `ack_latency = ack_ts - submit_ts` for each pair.
2. Fit lognormal: `mu = mean(ln(latency))`, `sigma = stdev(ln(latency))`,
   median = `exp(mu)`.
3. Estimate burst correlation: lag-1 autocorrelation of the
   `ln(latency)` series, clamped to `[0, 0.95]`.

For Empirical use, downsample the latency series (every Nth sample
or a histogram bucket) and pass it directly. The estimator
resamples with replacement.

## Notes

- The scalar `set_submit_ack_latency` setter is preserved and maps
  to `Uniform(base - jitter, base + jitter)`. Existing tests keep
  passing.
- Distributions are copied on attach. Mutating a distribution after
  attaching does not affect the executor — call
  `set_*_latency_distribution` again to update.
- Empirical sampling uses replacement, so finite samples cannot
  exhaust the source histogram.
