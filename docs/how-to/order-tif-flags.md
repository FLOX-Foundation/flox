# Extended time-in-force and reduce-only

The simulator now honours four order flags that real venues
enforce on submit. Strategies built against GTC and POST_ONLY get
the same behaviour as before; the new flags add coverage for the
order types professional venues (Binance UM, Deribit, Bybit
options) accept.

## Flags

| Flag          | Behaviour at submit                                                                                                              |
|---------------|----------------------------------------------------------------------------------------------------------------------------------|
| `gtc` (default) | Rests until cancelled or filled.                                                                                                |
| `ioc`         | Take whatever crosses now, cancel the remainder. Never rests.                                                                    |
| `fok`         | Atomic: fill the entire order at the best price or reject with `fok_not_fillable`. No partial fills.                              |
| `gtd`         | Like `gtc`, plus auto-cancel at `expires_at_ns`. Expiry fires on the next market event after the deadline.                         |
| `post_only`   | Reject any limit that would cross. Same as before this task.                                                                    |
| `reduce_only` (orthogonal flag) | Order may only reduce the open position. Rejected if it would open/grow; truncated if it would overshoot flat.   |

The TOB liquidity assumption for FOK is a simplification — only the
best level is consulted. Deeper-walk FOK is a follow-up.

### FOK mode (any-price vs single-price)

Real venues differ on what FOK means when crossing liquidity sits at
a different price than the order's limit:

| Mode           | Behaviour                                                  | Used by                       |
|----------------|------------------------------------------------------------|-------------------------------|
| `any_price`    | Fill if TOB qty ≥ order qty and TOB crosses the limit.      | Default; most crypto venues   |
| `single_price` | Fill only if TOB price equals the order's limit and TOB qty ≥ order qty. | CME, Eurex, most US equities  |

Single-price rejects with `fok_unfillable` when crossing liquidity
sits at a more aggressive level than the limit. `any_price` accepts
that same scenario.

=== "Python"

    ```python
    exec.set_fok_mode("single_price")
    print(exec.fok_mode())  # 'single_price'
    ```

=== "Node.js"

    ```javascript
    exec.setFokMode("single_price");
    exec.fokMode();  // 'single_price'
    ```

## Apply from a strategy

=== "Python"

    ```python
    --8<-- "examples/python_extended_tif.py"
    ```

=== "Node.js"

    ```javascript
    exec.submitOrder(1, 'buy', 50001.0, 1.0, 'limit', 1, { tif: 'fok' });
    exec.submitOrder(2, 'sell', 50000.0, 0.5, 'limit', 1,
                      { tif: 'ioc', reduceOnly: true });
    ```

=== "Codon"

    ```python
    exec.submit_order_ex(1, "buy", 50001.0, 1.0, "limit", 1, "fok")
    exec.submit_order_ex(2, "sell", 50000.0, 0.5, "limit", 1, "ioc",
                         reduce_only=True)
    ```

=== "QuickJS"

    ```javascript
    __flox_simulated_executor_submit_ex(exec, 1n, 0, 50001.0, 1.0, 1, 1, 2, 0, 0n);
    // tif=2 (FOK), reduce_only=0
    ```

=== "C++"

    ```cpp
    flox::Order o{};
    o.id = 1; o.side = flox::Side::BUY;
    o.price = flox::Price::fromDouble(50001.0);
    o.quantity = flox::Quantity::fromDouble(1.0);
    o.type = flox::OrderType::LIMIT;
    o.timeInForce = flox::TimeInForce::FOK;
    o.flags.reduceOnly = 1;
    sim.submitOrder(o);
    ```

## Reduce-only mechanics

The simulator maintains a per-symbol net position internally,
updated in `executeFill`. A reduce-only submit is evaluated against
that net:

- Flat → reject with `reduce_only`.
- Same side as the position → reject (would grow).
- Opposite side that exactly reduces or zeros → accepted.
- Opposite side that would flip the sign → truncated to flat.

The truncation is applied before the order enters the book / queue
tracker, so the rest of the lifecycle (fills, queue position, etc.)
operates on the truncated quantity.

## GTD expiry timing

`expires_at_ns` is an absolute timestamp in the simulator's clock
domain. The expiry fires at the next market event (book update /
trade) past the deadline — there is no internal tick-driven
scheduler. For deterministic expiry at exact times, advance the
clock and fire any market event after the deadline.

## Notes

- POST_ONLY already rejected crossing limits before this task; the
  behaviour is unchanged.
- IOC partial fills emit `PARTIALLY_FILLED` for the taken portion;
  the unfilled remainder emits `CANCELED` without a free-text
  reason field on cancel (the TIF tag is on the order itself).
- The C ABI ships `flox_simulated_executor_submit_order_ex` for
  the extended-flag path. The legacy `submit_order` stays
  byte-compatible.
