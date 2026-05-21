# Model order replace acknowledgement

Like cancellation, an order replace request takes time to acknowledge
at the exchange. The simulator can model that delay so a backtest
can reproduce the late-replace-after-fill race: the original order
filling in the window between replaceOrder and the venue's ack.

## Events

Async replace produces a four-event sequence on success:

- REPLACE_SUBMITTED — fires immediately on replaceOrder()
- REPLACE_ACCEPTED — fires at the sampled ack deadline
- REPLACED — terminal "old gone, new alive"

If the original order fills before the ack arrives:

- FILLED — on the original order
- REPLACE_REJECTED — with reject_reason "late_replace_after_fill"

REPLACED still fires through the existing onOrderReplaced listener;
the new in-flight statuses dispatch through three new virtuals
onOrderReplaceSubmitted, onOrderReplaceAccepted, onOrderReplaceRejected.
On the strategy callback surface, the in-flight events flow through
the existing on_order_update / onOrderUpdate path.

## Configure

Two new BacktestConfig knobs:

- replaceAckLatencyNs — base ack delay. Default 0 preserves the
  legacy synchronous replaceOrder().
- replaceAckJitterNs — uniform jitter band, clamped non-negative.

RNG sampling shares cancelAckSeed.

=== "Python"

    ```python
    import flox_py as flox

    runner = flox.BacktestRunner(
        registry,
        fee_rate=0.0,
        initial_capital=100_000.0,
        replace_ack_latency_ns=10_000_000,
        replace_ack_jitter_ns=2_000_000,
    )
    ```

=== "C++"

    ```cpp
    flox::BacktestConfig cfg;
    cfg.replaceAckLatencyNs = 10'000'000;
    cfg.replaceAckJitterNs  =  2'000'000;
    flox::BacktestRunner runner(cfg);
    ```

## Detect the race from a strategy

=== "Python"

    ```python
    import flox_py as flox

    class ReplaceWatcher(flox.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.replaces_lost = 0

        def on_order_update(self, ctx, ev):
            if (ev.status == "REPLACE_REJECTED" and
                ev.reject_reason == "late_replace_after_fill"):
                self.replaces_lost += 1
    ```

=== "Node.js"

    ```javascript
    const strat = {
      onOrderUpdate(ctx, ev) {
        if (ev.status === "REPLACE_REJECTED" &&
            ev.rejectReason === "late_replace_after_fill") {
          replacesLost += 1;
        }
      },
    };
    ```

## Notes

- The pending replace is dropped if the order is canceled by other
  means before the ack arrives. Replace ack finalization runs on
  the next simulator tick after the deadline passes.
- Conditional orders (stops, take-profits, trailing) use the same
  async path when replaceAckLatencyNs > 0.
- The clock source is the engine's simulated clock; live runs
  follow whichever clock the connector attached.
