# Estimate queue position from live trade + book events

Exchanges don't publish per-order queue position, but the value can
be approximated client-side from the order book and trade tape.
`LiveQueuePositionEstimator` ships the same arithmetic the backtest
simulator uses, fed by live events instead of synthetic ones — so
research code can read queue-ahead the same way under both
backtest and live, with the caveat that the live value is a
heuristic.

## How the estimate is built

At placement time, the estimator records the level total. As
events arrive:

- Trades at our price level deduct consumed volume from queue-ahead.
- Level shrinks that *exceed* trade-explained volume are attributed
  to cancellations, using the same proportional-shrink heuristic
  the backtest simulator uses.
- Our own fills are recorded separately so they're not counted as
  competing flow.

`queue_ahead_est = max(0, level_qty_at_arrival - consumed_by_trades
                          - proportional_shrink_share)`

This is a heuristic, not a measurement. The exact ordering of
cancellations vs new joins is hidden, so estimates can drift.

## Confidence

Each snapshot ships a `confidence` value in `[0, 1]`. It starts at
1.0 at placement and decays:

- Time decay: `confidence *= exp(-elapsed * ln(2) / halfLife)`.
  Default half-life is 60 seconds; tune via
  `set_confidence_half_life_ns`.
- Each proportional-shrink attribution multiplies confidence by
  `shrink_attribution_factor` (default 0.85). Trade-attributed
  deductions don't drop confidence — we know what happened.

Use confidence to size queue-position-conditioned bets: a 0.95
confidence is a strong signal; below 0.3 the estimate is roughly
"the queue churned a lot since you joined, no idea where you sit".

## Apply from a strategy

=== "Python"

    ```python
    --8<-- "examples/python_live_queue_position.py"
    ```

=== "Node.js"

    ```javascript
    const flox = require('flox-node');

    const est = new flox.LiveQueuePositionEstimator();
    est.setConfidenceHalfLifeNs(60_000_000_000);
    est.onOrderPlaced(symbol, 0, 50000.0, /*orderId=*/42, 0.5, 2.0, /*tsNs=*/0);

    // Wire to your engine's trade + book + execution events:
    engine.on('trade', t => est.onTrade(t.symbol, t.price, t.qty, t.tsNs));
    engine.on('levelUpdate', u =>
      est.onLevelUpdate(u.symbol, u.side, u.price, u.newQty, u.tsNs));
    engine.on('fill', f =>
      est.onOrderFilled(f.orderId, f.cumulativeFill, f.tsNs));

    const snap = est.snapshot(42);  // { queueAheadEst, confidence, ... }
    ```

=== "Codon"

    ```python
    from flox.live_queue_position import LiveQueuePositionEstimator

    est = LiveQueuePositionEstimator()
    est.set_confidence_half_life_ns(60_000_000_000)
    est.on_order_placed(symbol=1, side=0, price=50000.0, order_id=42,
                        order_qty=0.5, level_qty_now=2.0)
    est.on_trade(symbol=1, price=50000.0, qty=1.0, ts_ns=1_000_000_000)
    snap = est.snapshot(42)  # LiveQueueSnapshot or None
    ```

=== "QuickJS"

    ```javascript
    const h = __flox_live_queue_position_create();
    __flox_live_queue_position_on_order_placed(
        h, symbol, 0, 50000.0, 42n, 0.5, 2.0, 0n);
    __flox_live_queue_position_on_trade(h, symbol, 50000.0, 1.0, 1000000000n);
    const snap = __flox_live_queue_position_snapshot(h, 42n, 0n);
    ```

=== "C++"

    ```cpp
    #include "flox/execution/live_queue_position_estimator.h"
    flox::LiveQueuePositionEstimator est;
    est.onOrderPlaced(symbol, flox::Side::BUY, flox::Price::fromDouble(50000.0),
                      /*orderId=*/42, flox::Quantity::fromDouble(0.5),
                      flox::Quantity::fromDouble(2.0), /*tsNs=*/0);
    est.onTrade(symbol, flox::Price::fromDouble(50000.0),
                flox::Quantity::fromDouble(1.0), 1'000'000'000LL);
    auto snap = est.snapshot(/*orderId=*/42);  // std::optional<LiveQueueSnapshot>
    ```

## Snapshot fields

`snapshot(order_id, now_ns=0)` returns `None` when the order is not
tracked, otherwise a dict (Python) / object (Node) with:

| Python key | Node field | Meaning |
|---|---|---|
| `order_id` | `orderId` | The tracked order |
| `queue_ahead_est` | `queueAheadEst` | Estimated volume ahead in the queue |
| `total` | `total` | Level total recorded at placement |
| `confidence` | `confidence` | Decayed confidence in `[0, 1]` |
| `last_update_ns` | `lastUpdateNs` | Timestamp of the last event applied |
| `hidden_volume_seen` | `hiddenVolumeSeen` | Trade volume attributed to hidden liquidity |

`tracked_order_count()` reports how many orders the estimator is
currently following.

## Hidden and iceberg liquidity

Hidden fills that a naive estimator books as visible-queue consumption
are the main source of drift. `set_hidden_order_policy(policy)`
selects how trades are attributed:

| Policy | Behaviour |
|---|---|
| `ignore` (default) | Every trade deducts visible queue. Over-attributes when hidden flow is present. |
| `trust_trade_flag` | The caller flags hidden trades; flagged volume neither deducts queue nor feeds the proportional-shrink path. For venues that publish a per-trade hidden flag. |
| `infer_if_trade_exceeds_visible` | Trade volume above the last-known visible level total is attributed to hidden flow. For venues that do not flag hidden but where the excess is observable. |

Under `trust_trade_flag`, feed trades through
`on_trade_with_flag(symbol, price, qty, ts_ns=0, is_hidden=False)`
instead of `on_trade`. Attributed volume accumulates in the
snapshot's `hidden_volume_seen`.

Two more lifecycle hooks matter for accuracy:

- `on_order_cancelled(order_id, ts_ns=0)` — drop a cancelled order
  from tracking so it stops consuming a slot.
- `set_shrink_attribution_factor(factor)` — the confidence multiplier
  applied on each proportional-shrink attribution (default 0.85).

## Calibration

The shipped defaults (60s half-life, 0.85 shrink factor) are a
starting point. Calibrating requires real venue data:

- Place a small order, record `queue_ahead_est` over time, observe
  the actual fill outcome.
- For venues that DO publish queue position (some Eurex / Cboe
  feeds), compare estimator output against ground truth and tune
  the shrink factor until residuals minimize.
- Faster venues (Binance, Bybit) generally need a shorter time
  half-life — book churn is high.

Calibration is researcher-side; the engine does not ship venue
profiles for this estimator. File an issue if you have measured
values for a venue you want to share.

## Limits

- Hidden / iceberg orders are invisible to clients. Under the default
  `ignore` policy the proportional-shrink heuristic misattributes
  their fills as cancellations; the two other hidden-order policies
  above reduce but do not eliminate the error.
- Same-price joins from new orders show up as level growth, which
  the estimator treats as "behind us" — correct for queue position
  but understates how much competing volume the level now holds.
- Multi-tick orders (orders whose level changes via a market move)
  reset their queue position; the estimator currently drops them
  rather than tracking across levels. A future enhancement is on
  file.
