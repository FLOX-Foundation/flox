# Detect when a resting limit order is alone at its price level

The MarketPosition state `level_empty` reports when our resting
limit order is the only quantity left at its price. The detector
relies on the queue tracker's view of non-our quantity at the level
and works under both TOB and FULL queue models.

## When it fires

`level_empty` fires when:

1. The queue tracker is enabled (cfg.queueModel != NONE)
2. The non-our quantity at our exact price is zero
3. Either we are at best on our side, or no level on our side exists
   in the book (we are the only quote there)

The detector handles the common case where a level "vanishes" from
the book snapshot — providers usually drop levels at zero quantity
rather than reporting `qty=0`. The simulator now zeroes out any
tracked level that disappears from the next book snapshot so the
queue tracker stays in sync.

## Configure

Just enable the queue tracker — no extra config:

=== "Python"

    ```python
    executor.set_queue_model("tob")  # or "full"
    ```

=== "C++"

    ```cpp
    flox::BacktestConfig cfg;
    cfg.queueModel = flox::QueueModel::TOB;
    ```

## React from a strategy

=== "Python"

    ```python
    class Watcher(flox.Strategy):
        def on_market_position_change(self, ctx, ev):
            if ev.market_position == "level_empty":
                # alone at our price — reprice tighter or step back
                pass
    ```

## Notes

- Under `queue_model=none` the detector returns `unknown` for
  level-empty queries; you need TOB or FULL to get this signal.
- "Non-our quantity" is tracked from the book snapshots you feed
  into the executor. The simulator never publishes our own resting
  orders to its own book, so the level's `totalQty` represents
  other resting participants only.
- The state is exclusive with `best`: if we are at best and no
  others share our level, we are `level_empty`, not `best`. Both
  imply "best", but `level_empty` is the tighter classification.
