# Read queue position from a strategy

Limit orders resting on a simulated book carry a queue position that
shifts as trades consume volume in front of them and as the level
shrinks under cancels. This page shows how to observe that movement
from a strategy.

## What's exposed

For every fill or order-update event delivered to a strategy, the
event payload carries two extra fields:

- `queue_ahead` (Python / Codon) / `queueAhead` (Node / QuickJS) —
  volume ahead of the order at its price level at the time of the
  event
- `queue_total` / `queueTotal` — total quantity at the order's price
  level

A new event status, `QUEUE_POSITION_UPDATED`, fires when only the
queue position changed (no fill, no cancel, no replace). It dispatches
to an `on_queue_position_change` / `onQueuePositionChange` callback
so strategies can react without subscribing to every order update.

## Configure the emission threshold

A naive backtest would emit a queue-position event on every book
tick, which is too chatty for high-frequency books. The simulator
suppresses changes smaller than a configurable fraction of the
order's volume-at-arrival.

=== "Python"

    ```python
    import flox_py as flox

    ex = flox.SimulatedExecutor()
    ex.set_queue_model("tob")
    ex.set_queue_position_min_change_fraction(0.05)   # 5%
    ```

=== "Node.js"

    ```javascript
    const ex = new flox.SimulatedExecutor();
    ex.setQueueModel("tob");
    ex.setQueuePositionMinChangeFraction(0.05);
    ```

=== "C++"

    ```cpp
    flox::BacktestConfig cfg;
    cfg.queueModel = flox::QueueModel::TOB;
    cfg.queuePositionMinChangeFraction = 0.05;
    ```

Set the fraction to `0.0` to fire on every change (lossless, very
chatty) or to `1.0` to suppress queue-position events entirely.

## Observe queue position from a strategy

=== "Python"

    ```python
    import flox_py as flox

    class QueueWatcher(flox.Strategy):
        def on_queue_position_change(self, ctx, ev):
            ratio = ev.queue_ahead / ev.queue_total if ev.queue_total > 0 else 0.0
            if ratio > 0.8:
                # too far back, cancel and reprice
                self.cancel(ev.order_id)
    ```

=== "Node.js"

    ```javascript
    const strat = {
      onQueuePositionChange(ctx, ev, emit) {
        const ratio = ev.queueTotal > 0 ? ev.queueAhead / ev.queueTotal : 0;
        if (ratio > 0.8) {
          emit.cancel(ev.orderId);
        }
      },
    };
    ```

=== "QuickJS"

    ```javascript
    class QueueWatcher extends Strategy {
      onQueuePositionChange(ctx, ev) {
        const ratio = ev.queueTotal > 0 ? ev.queueAhead / ev.queueTotal : 0;
        if (ratio > 0.8) {
          this.cancel({ orderId: ev.orderId });
        }
      }
    }
    ```

=== "Codon"

    ```python
    from flox.strategy import Strategy

    class QueueWatcher(Strategy):
        def on_queue_position_change(self, ctx, ev):
            ratio = ev.queue_ahead / ev.queue_total if ev.queue_total > 0 else 0.0
            if ratio > 0.8:
                self.cancel(ev.order_id)
    ```

## Notes

- Queue position is a backtest-only signal. Live exchanges do not
  publish queue position; the fields read as `0` on live order
  events.
- Fill events (`PARTIALLY_FILLED` / `FILLED`) also carry the queue
  snapshot at fill time, so `on_fill` handlers can record where in
  the queue the fill landed.
- The threshold is computed against the order's `aheadAtArrival`
  snapshot, so a single fractional setting applies uniformly to
  orders of any size.
