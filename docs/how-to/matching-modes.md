# Choose a matching model: FIFO, pro-rata, hybrid

flox's queue simulator decides how an incoming trade at a price level
gets distributed across resting limit orders. The default is FIFO —
the order at the front of the queue eats first. Real venues do not
all behave that way.

| Venue family                       | Real matching        | Closest model      |
|------------------------------------|----------------------|--------------------|
| Most spot crypto, US equities      | Price-time FIFO      | `tob` / `full`     |
| Options exchanges (CME, Eurex)     | Pure pro-rata        | `pro_rata`         |
| Some hybrid futures / fee tiers    | FIFO top-N, pro-rata | `pro_rata_with_fifo` |

A pro-rata venue splits the trade across every resting order at the
level, weighted by order size. The front of the queue carries no
advantage — a 100-lot at the back gets the same proportional share as
the 100-lot at the front. Backtesting a market-maker strategy on a
pro-rata venue with the FIFO model overstates fills for big orders
and understates them for small ones.

`pro_rata_with_fifo` is the hybrid: the first N orders consume the
trade in FIFO order, and only the remainder is distributed pro-rata
across the rest. Several exchanges use this scheme to reward queue
priority while still preserving size-weighted matching for the bulk
of the book.

## Configure

The setter is on `SimulatedExecutor`. Set the model once at
strategy startup; switching mid-run is allowed but only affects
orders submitted after the call.

=== "Python"

    ```python
    import flox

    exec = flox.SimulatedExecutor()

    # Pure pro-rata.
    exec.set_queue_model("pro_rata", depth=4)

    # Or: FIFO top-3 then pro-rata across the rest.
    exec.set_queue_model("pro_rata_with_fifo", depth=4)
    exec.set_queue_fifo_top_n(3)
    ```

=== "TypeScript (Node)"

    ```typescript
    import { SimulatedExecutor } from "flox";

    const exec = new SimulatedExecutor();

    exec.setQueueModel("pro_rata", 4);

    // Or hybrid:
    exec.setQueueModel("pro_rata_with_fifo", 4);
    exec.setQueueFifoTopN(3);
    ```

=== "TypeScript (QuickJS)"

    ```typescript
    const exec = new SimulatedExecutor();
    exec.setQueueModel("pro_rata", 4);
    exec.setQueueModel("pro_rata_with_fifo", 4);
    exec.setQueueFifoTopN(3);
    ```

=== "Codon"

    ```python
    from flox.backtest import SimulatedExecutor, QueueModel

    exec = SimulatedExecutor()
    exec.set_queue_model(int(QueueModel.PRO_RATA), 4)
    exec.set_queue_model(int(QueueModel.PRO_RATA_WITH_FIFO), 4)
    exec.set_queue_fifo_top_n(3)
    ```

=== "C"

    ```c
    FloxSimulatedExecutorHandle exec = flox_simulated_executor_create();
    flox_simulated_executor_set_queue_model(exec, FLOX_QUEUE_PRO_RATA, 4);
    flox_simulated_executor_set_queue_model(exec, FLOX_QUEUE_PRO_RATA_WITH_FIFO, 4);
    flox_simulated_executor_set_queue_fifo_top_n(exec, 3);
    ```

A worked example (Python):

```python
--8<-- "examples/python_pro_rata_matching.py"
```

## What the simulator guarantees

- Pro-rata distribution is rounded down to the nearest raw unit, so
  the sum of fills never exceeds the trade quantity.
- A trade larger than the level total fills only the level total —
  no overshoot.
- An empty `fifoTopN` (zero or larger than the level depth) makes
  `pro_rata_with_fifo` behave identically to pure `pro_rata`.
- The `setFifoTopN` value persists across `set_queue_model` calls;
  reset it explicitly when switching back to a pure FIFO mode.

## When the model matters

Match the model to the venue you intend to trade. A bad choice can
push a backtest's hit rate off by 20–40% on the same tape:

- A maker strategy that posts size deep in the book looks profitable
  on FIFO (it rarely fills) but takes adverse fills on pro-rata.
- A queue-jumping strategy that posts small orders at the front loses
  most of its edge on pro-rata venues — its priority is worth less.

Treat the matching model as a first-class venue parameter alongside
fees, latency, and tick size.
