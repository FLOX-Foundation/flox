# Model cancellation ack latency

Real exchanges take time to acknowledge a cancel request. The
simulator can model that delay so backtests can reproduce the
late-cancel-after-fill race: an order that gets hit by an aggressive
trade in the window between `cancelOrder()` and the venue's ack.

## What's exposed

Three `BacktestConfig` fields on the C++ side:

- `cancelAckLatencyNs` — base ack delay in nanoseconds. Default `0`
  preserves the legacy synchronous behavior (`cancelOrder()` →
  `CANCELED` in the same tick).
- `cancelAckJitterNs` — uniform jitter band added to the base. The
  sampled latency lands in `[base - jitter, base + jitter]` and is
  clamped to a non-negative value.
- `cancelAckSeed` — RNG seed for reproducible sampling. Default `42`.

The bindings do not take these through `BacktestConfig` or the
`BacktestRunner` constructor. They configure the model on a
`SimulatedExecutor` instead:

- `set_cancel_ack_latency(latency_ns, jitter_ns=0)` — base + jitter.
- `set_cancel_ack_latency_distribution(dist)` — a `LatencyDistribution`
  (`constant`, `uniform`, `lognormal`, `empirical`, plus
  `set_burst_correlation`) instead of the base/jitter pair.
- `apply_latency_profile(name)` — a named venue profile:
  `binance_um_futures`, `bybit_linear`, `okx_swap`, `deribit`,
  `idealized`, `adversarial`.

`cancelAckSeed` has no binding setter — it is C++-only, so from Python
and Node the sampling uses the default seed and is reproducible across
runs but not tunable.

When `cancelAckLatencyNs > 0`, the simulator emits `PENDING_CANCEL`
immediately and defers `CANCELED` until simulation time reaches the
sampled ack deadline. In the meantime the order stays in the book
and can still fill.

If the order fills before the ack arrives, the simulator emits
`REJECTED` with `reject_reason = "late_cancel_after_fill"` on the
cancel attempt; the fill itself fires normally.

## Configure the model

=== "Python"

    ```python
    import flox_py as flox

    ex = flox.SimulatedExecutor()
    ex.set_cancel_ack_latency(10_000_000, 2_000_000)   # 10 ms base, ±2 ms jitter

    # Or a distribution instead of base/jitter:
    ex.set_cancel_ack_latency_distribution(
        flox.LatencyDistribution.lognormal(10_000_000, 0.4))

    # Or a named venue profile (sets submit / cancel / replace together):
    ex.apply_latency_profile("binance_um_futures")
    ```

=== "Node.js"

    ```javascript
    const ex = new flox.SimulatedExecutor();
    ex.setCancelAckLatency(10_000_000, 2_000_000);

    const dist = new flox.LatencyDistribution();
    dist.setLognormal(10_000_000, 0.4);
    ex.setCancelAckLatencyDistribution(dist);

    ex.applyLatencyProfile("binance_um_futures");
    ```

=== "C++"

    ```cpp
    flox::BacktestConfig cfg;
    cfg.cancelAckLatencyNs = 10'000'000;
    cfg.cancelAckJitterNs = 2'000'000;
    cfg.cancelAckSeed = 42;
    flox::BacktestRunner runner(cfg);
    ```

## Detect a lost-to-fill race from a strategy

=== "Python"

    ```python
    import flox_py as flox

    class CancelWatcher(flox.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.fills_lost = 0

        def on_order_update(self, ctx, ev):
            if (ev.status == "REJECTED" and
                ev.reject_reason == "late_cancel_after_fill"):
                self.fills_lost += 1
    ```

=== "Node.js"

    ```javascript
    const strat = {
      onOrderUpdate(ctx, ev) {
        if (ev.status === "REJECTED" &&
            ev.rejectReason === "late_cancel_after_fill") {
          fillsLost += 1;
        }
      },
    };
    ```

## Notes

- The clock source is the engine's simulated clock. Cancel ack
  finalization runs on the next simulator tick (book update, trade,
  or bar) after the sampled deadline has passed. If no ticks occur,
  the cancel sits in the pending queue.
- For `cancelAllOrders(symbol)`, every matching order is enqueued
  separately and receives an independently sampled ack delay.
- Conditional orders (stops, take-profits, trailing) are cancelled
  through the same async path when `cancelAckLatencyNs > 0`.
