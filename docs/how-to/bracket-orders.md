# Submit a native bracket order

A bracket is a single venue primitive that wraps three legs:

- An **entry** order (limit or stop) that opens the position.
- A **take-profit** child that closes at a price better than entry.
- A **stop** child that closes at a price worse than entry.

The venue ties them together: take-profit and stop only arm after
entry fills, and the first child to fill cancels the other. Most
modern derivatives venues (Bybit, OKX, Binance UM with "TP/SL on
position") offer this as one API call.

flox's `submitBracket` exposes the same surface in the simulator.
You can hand-wire the three legs via `OrderGroup` for more
flexibility, but the bracket primitive is the right tool when you
just want the standard pattern with no extra wiring.

## Configure

A worked example (Python):

```python
--8<-- "examples/python_bracket_orders.py"
```

=== "Python"

    ```python
    import flox_py as flox

    exec = flox.SimulatedExecutor()
    exec.submit_bracket(bracket_id=1, symbol=1,
                         entry_side="buy", entry_type="limit",
                         entry_price=50000.0, quantity=0.1,
                         tp_side="sell", tp_type="limit", tp_price=51000.0,
                         stop_side="sell", stop_type="stop_market",
                         stop_trigger_price=49500.0)
    state = exec.bracket_state(1)
    ```

=== "TypeScript (Node)"

    ```typescript
    import { SimulatedExecutor } from "flox";

    const exec = new SimulatedExecutor();
    exec.submitBracket({
      bracketId: 1,
      entrySide: "buy", entryType: "limit",
      entryPrice: 50000.0, quantity: 0.1,
      tpSide: "sell", tpType: "limit", tpPrice: 51000.0,
      stopSide: "sell", stopType: "stop_market",
      stopTriggerPrice: 49500.0,
    });
    const state = exec.bracketState(1);
    ```

=== "TypeScript (QuickJS)"

    ```typescript
    const exec = new SimulatedExecutor();
    exec.submitBracket({
      bracketId: 1,
      entrySide: "buy", entryType: "limit",
      entryPrice: 50000.0, quantity: 0.1,
      tpSide: "sell", tpType: "limit", tpPrice: 51000.0,
      stopSide: "sell", stopType: "stop_market",
      stopTriggerPrice: 49500.0,
    });
    ```

=== "Codon"

    ```python
    from flox.backtest import SimulatedExecutor

    exec = SimulatedExecutor()
    exec.submit_bracket(1, 1,
                        "buy", "limit", 50000.0, 0.1,
                        "sell", "limit", 51000.0,
                        "sell", "stop_market", 49500.0)
    ```

=== "C"

    ```c
    FloxSimulatedExecutorHandle exec = flox_simulated_executor_create();
    flox_simulated_executor_submit_bracket(
        exec, /*bracket_id=*/1, /*symbol=*/1,
        /*entry_side=*/0, /*entry_type=*/0, 50000.0, 0.1,
        /*tp_side=*/1,    /*tp_type=*/0,    51000.0,
        /*stop_side=*/1,  /*stop_type=*/2,  49500.0);
    ```

## State machine

The bracket transitions through five states:

| state           | meaning                                          |
|-----------------|--------------------------------------------------|
| pending_entry   | entry submitted; not filled yet                  |
| entry_filled    | entry fully filled; TP + stop are now armed      |
| tp_filled       | take-profit filled; stop was cancelled           |
| stop_filled     | stop filled / triggered; take-profit was cancelled|
| canceled        | bracket cancelled before children resolved       |

`cancelBracket` cancels every leg that is still live, regardless of
state.

## OrderId derivation

To keep the API simple, leg OrderIds are derived from the
`bracketId`:

- entry: `bracketId * 3 + 0`
- take-profit: `bracketId * 3 + 1`
- stop: `bracketId * 3 + 2`

If your strategy assigns OrderIds manually, allocate `bracketId`s
out of a space that doesn't collide with the manually-issued
OrderIds. The simulator does not currently enforce this.

## Notes

- Each child's `quantity` matches the actual entry fill at the time
  TP + stop are submitted, so partial entry fills produce
  proportionally smaller children. Re-entering after partial fill
  on the entry leg is left to higher-level strategies.
- Bracket is not chainable: you cannot submit a new bracket whose
  entry order id collides with an existing one. Reuse a fresh
  `bracketId` per attempt.
- Trailing-stop variant is filed as a follow-up — it needs trailing
  logic in the engine, not just composition.
