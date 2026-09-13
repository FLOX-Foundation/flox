# Choose a matching model: FIFO, pro-rata, hybrid

flox's queue simulator decides how an incoming trade at a price level
gets distributed across resting limit orders. The default is `none`,
which skips queue modelling entirely; `tob` and `full` are the FIFO
models, where the order at the front of the queue eats first. Real
venues do not all behave that way.

| Venue family                       | Real matching                  | Closest model              |
|------------------------------------|--------------------------------|----------------------------|
| No queue modelling (the default)   | —                              | `none`                     |
| Most spot crypto, US equities      | Price-time FIFO                | `tob` / `full`             |
| Options exchanges (CME, Eurex)     | Pure pro-rata                  | `pro_rata`                 |
| Some hybrid futures / fee tiers    | FIFO top-N, pro-rata           | `pro_rata_with_fifo`       |
| CME Globex options (TOP-PRO-LMM)   | Top fixed share + pro-rata tail | `top_pro_lmm`              |
| ICE options size-pro-rata          | Pure pro-rata weighted by priority | `pro_rata_with_priority` |

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

## What `none` does

`none` is the default and it models no queue at all. A resting limit
order fills as soon as the opposite touch crosses its price, without
waiting for anything in front of it. The fill happens at the price the
order posted and is reported as a maker fill, the same as it would be
under `tob` or `full`.

That last part matters on coarse data. When the touch jumps past a
resting order between two observations — a 1-minute or 1-hour bar
rather than a tick — the order still trades at its own price. It does
not collect the distance the touch travelled. A limit order that
arrives already marketable is a different case: it crosses the book,
pays the touch and is reported as a taker fill.

`none` is fast and it gets the economics of a fill right. What it does
not give you is queue position: an order at the back of a deep level
fills just as readily as one at the front. Use `tob` or `full` when
queue priority is part of the strategy.

## Configure

The setter is on `SimulatedExecutor`. Set the model once at
strategy startup; switching mid-run is allowed but only affects
orders submitted after the call.

=== "Python"

    ```python
    import flox_py as flox

    exec = flox.SimulatedExecutor()

    # Pure pro-rata.
    exec.set_queue_model("pro_rata", depth=4)

    # Or: FIFO top-3 then pro-rata across the rest.
    exec.set_queue_model("pro_rata_with_fifo", depth=4)
    exec.set_queue_fifo_top_n(3)
    ```

=== "TypeScript (Node)"

    ```typescript
    import { SimulatedExecutor } from "@flox-foundation/flox";

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
    from flox.backtest import SimulatedExecutor

    # flox.backtest names only QUEUE_NONE / QUEUE_TOB / QUEUE_FULL. The
    # pro-rata models are the remaining FloxQueueModel values from the C
    # ABI, passed as plain ints.
    PRO_RATA = 3
    PRO_RATA_WITH_FIFO = 4

    exec = SimulatedExecutor()
    exec.set_queue_model(PRO_RATA, 4)
    exec.set_queue_model(PRO_RATA_WITH_FIFO, 4)
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

## TOP-PRO-LMM (CME Globex options)

The order at the front of the queue receives a fixed share of every
incoming trade (capped by its remaining), and the rest of the trade
distributes pro-rata across the tail. LMM (Lead Market Maker) orders
in the tail carry a bonus multiplier.

=== "Python"

    ```python
    exec.set_queue_model("top_pro_lmm", depth=4)
    exec.set_top_priority_share(0.40)        # TOP gets 40% of each trade
    exec.set_lmm_orders([order_id_a, order_id_b])
    exec.set_lmm_bonus_multiplier(1.5)        # LMM bonus
    # Optional per-order multiplier on top of LMM bonus:
    exec.set_order_priority_multiplier(order_id_a, 1.25)
    ```

=== "Node"

    ```javascript
    exec.setQueueModel("top_pro_lmm", 4);
    exec.setTopPriorityShare(0.40);
    exec.setLmmOrders([orderIdA, orderIdB]);
    exec.setLmmBonusMultiplier(1.5);
    ```

If the LMM list is empty, the tail distributes as pure pro-rata
weighted by the per-order priority multiplier (default 1.0).

## PRO_RATA_WITH_PRIORITY (ICE options)

Every order at the level gets an effective weight of
`remaining × priorityMultiplier`. Used to model ICE-style options
matching where pinned MM agreements carry a static priority
multiplier (e.g. 1.5) on top of raw size.

=== "Python"

    ```python
    exec.set_queue_model("pro_rata_with_priority", depth=4)
    exec.set_order_priority_multiplier(pinned_mm_id, 1.5)
    ```

=== "Node"

    ```javascript
    exec.setQueueModel("pro_rata_with_priority", 4);
    exec.setOrderPriorityMultiplier(pinnedMmId, 1.5);
    ```

## What the simulator guarantees

- Pro-rata distribution is rounded down to the nearest raw unit, so
  the sum of fills never exceeds the trade quantity.
- A trade larger than the level total fills only the level total —
  no overshoot.
- A trade consumes resting orders on one side of the level only: the
  side opposite the aggressor. A buyer who lifts the offer trades
  against resting asks, a seller who hits the bid trades against
  resting bids. Feed the aggressor flag along with the trade
  (`on_trade(symbol, price, qty, is_buy)`) and a two-sided quote
  cannot trade with itself on a single print.
- An empty `fifoTopN` (zero or larger than the level depth) makes
  `pro_rata_with_fifo` behave identically to pure `pro_rata`.
- The `set_queue_fifo_top_n` / `setQueueFifoTopN` value persists
  across `set_queue_model` calls; reset it explicitly when switching
  back to a pure FIFO mode.

## When the model matters

Match the model to the venue you intend to trade. A bad choice can
push a backtest's hit rate off by 20–40% on the same tape:

- A maker strategy that posts size deep in the book looks profitable
  on FIFO (it rarely fills) but takes adverse fills on pro-rata.
- A queue-jumping strategy that posts small orders at the front loses
  most of its edge on pro-rata venues — its priority is worth less.

Treat the matching model as a first-class venue parameter alongside
fees, latency, and tick size.
