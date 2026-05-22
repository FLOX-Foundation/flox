# Apply a named latency profile

`flox::LatencyProfiles` is a small library of named ack-latency
defaults calibrated for common venues. Each profile mutates a
`BacktestConfig` (C++) or applies to a `SimulatedExecutor` directly
(bindings) by name string.

## Available profiles

| name | submit ack p50 | cancel ack p50 | replace ack p50 |
|---|---:|---:|---:|
| `binance_um_futures` |  5 ms ± 3 |  8 ms ± 3 | 12 ms ± 4 |
| `bybit_linear`       |  7 ms ± 4 | 10 ms ± 4 | 14 ms ± 5 |
| `okx_swap`           |  6 ms ± 3 |  9 ms ± 3 | 13 ms ± 4 |
| `deribit`            | 12 ms ± 5 | 15 ms ± 5 | 20 ms ± 6 |
| `idealized`          |  0        |  0        |  0        |
| `adversarial`        | 100 ms ± 30 | 150 ms ± 30 | 200 ms ± 40 |

Numbers come from public exchange latency reports and observed p50
medians in 2025. They are starting points; tune from observed
latency in your own environment.

## Apply from a strategy

=== "Python"

    ```python
    --8<-- "examples/python_apply_latency_profile.py"
    ```

=== "Node.js"

    ```javascript
    const exec = new flox.SimulatedExecutor();
    exec.applyLatencyProfile("binance_um_futures");
    ```

=== "QuickJS"

    ```javascript
    __flox_simulated_executor_apply_latency_profile(handle, "binance_um_futures");
    ```

=== "Codon"

    ```python
    from flox.backtest import SimulatedExecutor
    exec = SimulatedExecutor()
    exec.apply_latency_profile("binance_um_futures")
    ```

=== "C++"

    ```cpp
    #include "flox/backtest/latency_profiles.h"
    flox::BacktestConfig cfg;
    flox::LatencyProfiles::binance_um_futures(cfg);
    flox::BacktestRunner runner(cfg);
    ```

## Notes

- Applying a profile touches only the three ack-latency knob pairs
  (submit / cancel / replace). Queue model, slippage profile, and
  other config fields stay as they were.
- Unknown profile names are silently no-op rather than throwing.
  Researchers should sanity-check by reading back the latency knobs
  after applying a profile in their first run.
- Profiles use static numbers, not live measurements. A calibration
  tool that learns from a tape is filed as a future enhancement.
