# Read maker / taker on a fill

Every fill event carries a flag indicating whether the order acted as
a maker (rested in the book and was consumed by an aggressive
opposite trade) or a taker (arrived marketable and crossed the book).

## What's exposed

For `PARTIALLY_FILLED` and `FILLED` events:

- Python / Codon: `ev.is_maker` (bool), `ev.fill_role` (`"maker"` |
  `"taker"`)
- Node / QuickJS: `ev.isMaker` (boolean), `ev.fillRole` (`"maker"` |
  `"taker"` | `null` for non-fill events)
- C ABI: `FloxOrderEventData.is_maker` (uint8, 0 = taker, 1 = maker)

Non-fill statuses always report `is_maker = false` / `fillRole = null`.

## How the simulator classifies

- A resting limit order whose queue position gets consumed by an
  aggressive trade in the opposite direction is **maker**.
- A market order, or a limit order that arrived marketable and
  crossed the book on submission, is **taker**.
- A marketable limit that partially crosses then rests will record
  the crossed fill as taker and any subsequent queue-consumption
  fills as maker.

## Use in a fee model

=== "Python"

    ```python
    import flox_py as flox

    MAKER_BPS = 1.0   # 1 bp rebate or fee depending on venue
    TAKER_BPS = 5.0

    class FeeTracker(flox.Strategy):
        def on_fill(self, ctx, ev):
            notional = ev.fill_qty * ev.fill_price
            fee = notional * (MAKER_BPS if ev.is_maker else TAKER_BPS) / 10_000
            self._total_fees += fee
    ```

=== "Node.js"

    ```javascript
    const strat = {
      onFill(ctx, ev) {
        const notional = ev.fillQty * ev.fillPrice;
        const bps = ev.isMaker ? 1.0 : 5.0;
        totalFees += notional * bps / 10_000;
      },
    };
    ```

=== "QuickJS"

    ```javascript
    class FeeTracker extends Strategy {
      onFill(ctx, ev) {
        const notional = ev.fillQty * ev.fillPrice;
        const bps = ev.isMaker ? 1.0 : 5.0;
        this.totalFees += notional * bps / 10_000;
      }
    }
    ```

=== "Codon"

    ```python
    from flox.strategy import Strategy

    class FeeTracker(Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.total_fees = 0.0

        def on_fill(self, ctx, ev):
            notional = ev.fill_qty * ev.fill_price
            bps = 1.0 if ev.is_maker else 5.0
            self.total_fees += notional * bps / 10_000
    ```

## Notes

- The classification is exposed as data; the simulator does not
  apply a fee model based on it. Strategies that need realistic fee
  bookkeeping should multiply through their own `MAKER_BPS` /
  `TAKER_BPS` values per venue tier.
- For live executors that forward exchange-side maker/taker flags,
  the field reflects whatever the venue reports.
