# Submit a native iceberg order

A native iceberg order is a venue primitive: trader sends a single
order with a visible slice size and the total quantity. The
exchange exposes only the visible slice on the book and
auto-refreshes the next slice from the hidden remainder as the
visible portion fills. Other participants see a sequence of small
fills and level refills, not one big resting order.

flox ships `IcebergExecutor` for client-side slicing (each leg pays
a round-trip latency). The simulator also supports the venue's
native iceberg primitive, with no per-refresh latency unless you
configure one.

## When to use which

| Scenario                                     | Pick                  |
|----------------------------------------------|-----------------------|
| Venue offers iceberg natively (CME, Eurex)   | `submit_iceberg`      |
| Client-controlled slicing on a venue without | `IcebergExecutor`     |
| Hybrid (some venues, some not, one strategy) | `submit_iceberg` then fall back to executor algo on rejection |

Native iceberg is preferred when available: the venue handles the
refresh atomically, no client-side network round-trip per slice.

## Configure

A worked example (Python):

```python
--8<-- "examples/python_iceberg_orders.py"
```

=== "Python"

    ```python
    import flox_py as flox

    exec = flox.SimulatedExecutor()
    # 1ms refresh delay (most venues are instant; leave at 0 for those).
    exec.set_iceberg_refresh_latency(1_000_000)
    exec.submit_iceberg(order_id=1, side="buy", price=50000.0,
                         total_quantity=20.0, visible_quantity=2.0)
    ```

=== "TypeScript (Node)"

    ```typescript
    import { SimulatedExecutor } from "flox";

    const exec = new SimulatedExecutor();
    exec.setIcebergRefreshLatency(1_000_000);
    exec.submitIceberg(1, "buy", 50000.0, 20.0, 2.0);
    ```

=== "TypeScript (QuickJS)"

    ```typescript
    const exec = new SimulatedExecutor();
    exec.setIcebergRefreshLatency(1_000_000);
    exec.submitIceberg(1, "buy", 50000.0, 20.0, 2.0);
    ```

=== "Codon"

    ```python
    from flox.backtest import SimulatedExecutor

    exec = SimulatedExecutor()
    exec.set_iceberg_refresh_latency(1_000_000)
    exec.submit_iceberg(1, "buy", 50000.0, 20.0, 2.0)
    ```

=== "C"

    ```c
    FloxSimulatedExecutorHandle exec = flox_simulated_executor_create();
    flox_simulated_executor_set_iceberg_refresh_latency(exec, 1000000);
    flox_simulated_executor_submit_iceberg(exec, /*id=*/1, /*side=*/0,
                                           /*price=*/50000.0,
                                           /*total=*/20.0, /*visible=*/2.0,
                                           /*symbol=*/1);
    ```

## Behaviour the simulator guarantees

- Only `visible_quantity` is added to the book queue at submit.
- When the visible tranche fully fills, the simulator atomically
  refreshes another slice of `min(visible_quantity, hidden_remainder)`
  unless a refresh latency is configured, in which case the next
  slice is exposed at `fill_time + latency` instead.
- The refreshed slice goes to the **back** of the queue at that
  price level — the venue treats each refresh as a new resting
  entry, not as one continuous order.
- Cancel cancels both the visible portion and the hidden remainder
  in one call.

## Inspect remaining hidden quantity

For diagnostics, the executor exposes the raw fixed-point hidden
remainder per order ID:

=== "Python"

    ```python
    rem_raw = exec.iceberg_hidden_remaining_raw(order_id=1)
    rem = rem_raw / 1e8  # Quantity is fixed-point with scale 1e-8
    ```

=== "TypeScript (Node)"

    ```typescript
    const remRaw = exec.icebergHiddenRemainingRaw(1);
    ```

The value drops by `visible_quantity` on each refresh and reaches 0
once the order is fully filled.

## Notes

- `visible_quantity` must be strictly less than `total_quantity` —
  otherwise the order behaves as a regular limit (no hidden state
  created).
- Refresh latency is a single venue-wide setting, not per-order.
  Most venues are instant; some inject 0.5-2ms between refresh and
  next-slice exposure.
- Self-trade prevention (T025) and rate-limit policy (T022) apply
  to native iceberg the same way they apply to limit orders.
