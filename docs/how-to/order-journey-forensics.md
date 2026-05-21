# Record and analyse order journeys

`OrderJourneyTracer` is an execution listener that records the full
event sequence of every order it observes. Attach it to a
`BacktestRunner` once and it captures every status transition with
queue position, timestamps, and maker/taker classification for
post-trade analysis.

## Use it

```python
--8<-- "examples/python_order_journey_forensics.py"
```

## What gets recorded

Each row carries:

- `order_id` and per-order `seq` (0-based)
- `status` — `OrderEventStatus` numeric code
- `ts_ns` — event emission timestamp
- `fill_qty`, `fill_price` — populated on fill events
- `queue_ahead`, `queue_total` — populated on fill and
  `QUEUE_POSITION_UPDATED` events
- `is_maker` — populated on fill events
- `submitted_at_ns` ... `expired_at_ns` — full per-stage timestamp
  snapshot

The output is a numpy structured array, so downstream analysis can use
`pandas.DataFrame(arr)` or numpy operations directly.

## Bounded memory

Two caps keep the tracer safe in long runs:

- `max_orders` (default 1,000,000) — once exceeded, the
  oldest tracked order (by first-seen time) is evicted whole.
- `max_records_per_order` (default 64) — extra events past this cap
  are dropped silently for the affected order.

## Sampling

`sample_rate` in `[0.0, 1.0]` selects orders deterministically:
`(order_id * sample_salt) % 1000` is compared against a threshold.
The same `sample_salt` produces the same selection across runs, so
results are reproducible.

## Notes

- The tracer is a research tool. It implements
  `IOrderExecutionListener` and ships as a Python binding only;
  Node / QuickJS / Codon parity is not enforced for this category of
  read-only post-trade utility (same precedent as `Hurst` and `ADF`).
- The tracer fires from the engine's order-event path, after the
  position tracker updates. Attached PnL trackers and other
  listeners see events in the same order.
- For live runs, the tracer accumulates events from the live
  connector's order callbacks too, but live-side queue position
  reads as zero (exchanges do not publish queue position).
