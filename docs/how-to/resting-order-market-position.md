# Track a resting order's market position

A resting limit order's place in the book changes as quotes move
around it: a better quote may appear ahead, the spread may widen
until the order sits mid-spread, the level may empty until only the
order remains. The simulator surfaces these categorical transitions
as `MARKET_POSITION_CHANGED` events.

## States

Five categorical states for any resting limit order:

- `best` — order is at the current best on its side
- `behind_best` — a better quote exists on the same side
- `mid_spread` — order price is strictly between best bid and best
  ask (neither side has a quote at our price)
- `level_empty` — no other quantity remains at the order's level
- `crossed` — order price crosses the opposite side; the simulator
  should have filled or rejected (used as a diagnostic)

Events fire only when the categorical state transitions. The
continuous `distance_to_best_ticks` field is available on every
event payload for strategies that want finer granularity.

## React from a strategy

=== "Python"

    ```python
    --8<-- "examples/python_market_position.py"
    ```

=== "Node.js"

    ```javascript
    const strat = {
      onMarketPositionChange(ctx, ev, emit) {
        if (ev.marketPosition === "behind_best") {
          emit.cancel(ev.orderId);
        }
      },
    };
    ```

=== "QuickJS"

    ```javascript
    class Watcher extends Strategy {
      onMarketPositionChange(ctx, ev) {
        if (ev.marketPosition === "behind_best") {
          this.cancel({ orderId: ev.orderId });
        }
      }
    }
    ```

=== "Codon"

    ```python
    from flox.strategy import Strategy

    class Watcher(Strategy):
        def on_market_position_change(self, ctx, ev):
            if ev.market_position == "behind_best":
                self.cancel(ev.order_id)
    ```

## Notes

- The state is recomputed after every book update and after every
  trade that may shift the top-of-book.
- `distance_to_best_ticks` is signed raw price units from best on
  our side. Positive means behind; negative means ahead of best
  (mid-spread or crossed). Strategies that care about ticks should
  divide by their tick size.
- Backtest only. Live exchanges do not generally publish enough book
  state to compute market position reliably on every tick from a
  client.
