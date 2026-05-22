# Attribute hidden / iceberg flow correctly in queue estimation

The proportional-shrink heuristic in
`LiveQueuePositionEstimator` (T020) cannot distinguish three
sources of level shrinkage:

1. Visible volume printed as a trade.
2. Visible volume cancelled (proportional-shrink path).
3. Hidden / iceberg fills — venue prints them as trades at the
   price level even though the visible book never showed that
   volume.

On venues with material hidden liquidity (Bybit some products,
OKX dark pools), category 3 gets mis-attributed by the default
estimator as either trade-consumed visible volume or cancelled
visible volume. Either path drains the estimator's queue-ahead
faster than reality, so resting maker orders look closer to the
front than they actually are.

`HiddenOrderPolicy` lets the caller choose one of three handling
modes per estimator instance.

## Modes

| Mode                              | Behaviour                                                                                                                                                                       |
|-----------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `ignore` (default)                | Every trade deducts visible queue. Original T020 behaviour.                                                                                                                       |
| `trust_trade_flag`                | Caller passes `is_hidden` flag on the trade. Flagged trades do not deduct queue or feed the proportional-shrink path; instead they accumulate into `hidden_volume_seen`.            |
| `infer_if_trade_exceeds_visible`  | When the reported trade volume exceeds the last-cached visible level total at that price, the excess is attributed to hidden flow; the visible portion deducts queue normally.    |

`hidden_volume_seen` is a cumulative diagnostic on the snapshot —
not subtracted from queue-ahead. Use it to size your confidence in
the estimator on venues with active hidden flow.

## Apply from a strategy

=== "Python"

    ```python
    --8<-- "examples/python_hidden_order_attribution.py"
    ```

=== "Node.js"

    ```javascript
    const flox = require('flox-node');
    const est = new flox.LiveQueuePositionEstimator();
    est.setHiddenOrderPolicy('trust_trade_flag');
    est.onTradeWithFlag(symbol, 50000.0, 1.0, tsNs, /*isHidden=*/true);
    ```

=== "Codon"

    ```python
    from flox.live_queue_position import LiveQueuePositionEstimator

    est = LiveQueuePositionEstimator()
    est.set_hidden_order_policy("trust_trade_flag")
    est.on_trade_with_flag(symbol=1, price=50000.0, qty=1.0,
                            ts_ns=ts, is_hidden=True)
    ```

=== "QuickJS"

    ```javascript
    __flox_live_queue_position_set_hidden_order_policy(handle, 1);
    __flox_live_queue_position_on_trade_with_flag(handle, sym, p, q, ts, 1);
    ```

=== "C++"

    ```cpp
    est.setHiddenOrderPolicy(flox::HiddenOrderPolicy::TrustTradeFlag);
    est.onTradeWithFlag(symbol, flox::Price::fromDouble(50000.0),
                        flox::Quantity::fromDouble(1.0), tsNs, /*isHidden=*/true);
    ```

## Venue flag availability

| Venue                  | Per-trade `is_hidden`                              |
|------------------------|----------------------------------------------------|
| Binance UM futures     | No                                                  |
| Bybit linear perps     | Partial — flag on some product lines               |
| OKX swap               | Inference path is the practical choice              |
| Deribit options        | No                                                  |

For venues without a flag, `infer_if_trade_exceeds_visible` is the
fallback. The trade-vs-visible delta is a conservative under-
attributor (uses the smaller of bid/ask cached visible), so it
under-counts rather than over-counts hidden flow.

## Notes

- Hidden-attributed deductions do not penalise confidence — the
  estimator knows it was a hidden fill, not a cancellation race.
- The estimator caches level totals via `on_order_placed` and
  `on_level_update`. For inference mode to work, the caller must
  feed level-update events; without them, the cached visible total
  stays at the placement-time value.
- `hidden_volume_seen` is per-order: it accumulates only for orders
  resting at the price level the trade hit.
