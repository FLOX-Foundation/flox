# Model submit-side ack latency

The simulator can defer the `ACCEPTED` event until a sampled
deadline after `submitOrder()`. This mirrors the cancel and replace
ack models and lets backtests reproduce the race where market data
moves between submission and the venue accepting the order.

## Behavior

- `SUBMITTED` fires immediately
- The order is held aside, not yet placed in the book
- At the sampled ack deadline: `ACCEPTED` fires + the order runs
  through the existing post-accept logic (POST_ONLY check, queue
  tracker registration, try-fill)
- If POST_ONLY would cross the book at acceptance time, the order
  is `REJECTED` with `reject_reason = "late_post_only_crossed"`

## Configure

Two `BacktestConfig` knobs:

- `submitAckLatencyNs` — base ack delay. Default `0` preserves the
  legacy synchronous flow.
- `submitAckJitterNs` — uniform jitter band, clamped non-negative.

RNG sampling shares `cancelAckSeed`.

The bindings expose the pair as one `SimulatedExecutor` setter,
`set_submit_ack_latency(latency_ns, jitter_ns=0)`. `BacktestRunner`'s
constructor takes only `(registry, fee_rate, initial_capital)` and
owns its executor internally, so from Python / Node the knob applies
to a `SimulatedExecutor` you drive yourself — a standalone one, or
`PaperBroker.simulated_executor`. Set `BacktestConfig` in C++ to
change the knob for a `BacktestRunner`.

=== "Python"

    ```python
    import flox_py as flox

    ex = flox.SimulatedExecutor()
    ex.set_submit_ack_latency(5_000_000, 2_000_000)   # 5 ms ± 2 ms
    ```

=== "Node.js"

    ```javascript
    const ex = new flox.SimulatedExecutor();
    ex.setSubmitAckLatency(5_000_000, 2_000_000);
    ```

=== "C++"

    ```cpp
    flox::BacktestConfig cfg;
    cfg.submitAckLatencyNs = 5'000'000;
    cfg.submitAckJitterNs  = 2'000'000;
    flox::BacktestRunner runner(cfg);
    ```

## Detect the race from a strategy

```python
--8<-- "examples/python_submit_latency_watcher.py"
```

## Notes

- The order does not participate in queue matching or fills before
  `ACCEPTED`. Trades arriving during the ack window do not consume
  it.
- Conditional orders (stop / TP / trailing) follow the same async
  path when `submitAckLatencyNs > 0`. They reach
  `PENDING_TRIGGER` at the ack deadline, not at submit time.
- The clock source is the engine's simulated clock; live runs
  follow whichever clock the connector attached.
