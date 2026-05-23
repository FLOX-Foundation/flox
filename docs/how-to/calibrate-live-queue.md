# Calibrate the live queue position estimator

`LiveQueuePositionEstimator` carries two tunables — a confidence
half-life and a per-event shrink factor — that govern how its
`confidence` value drifts. The defaults (60s, 0.85) are reasonable
starting points but venue-specific. Without calibration against
ground truth, `confidence` is a vibe metric.

The `LiveQueueCalibrator` toolkit (Python-only research surface)
fits those two knobs against either:

1. **Venue-published queue position** — some venues (Eurex, Cboe
   options) publish queue position via private API. Feed the
   `(estimator_value, ground_truth, elapsed_ns)` triples directly.
2. **Test-order roundtrip** — place a small known-size order, then
   record the observed queue-ahead at fill. The miscalibration
   residual is the signal the fit minimises.

## Configure

A worked example (Python):

```python
--8<-- "examples/python_live_queue_calibration.py"
```

### Sample mode

When you can pull ground truth from the venue:

```python
import flox_py as flox

estimator = flox.LiveQueuePositionEstimator()
cal = flox.LiveQueueCalibrator(estimator)

# (per observation)
cal.record_sample(order_id=42,
                   estimator_value=14.3,
                   ground_truth=12.8,
                   elapsed_ns=4_500_000_000,
                   shrink_events=2)

result = cal.fit()
cal.apply(result)             # update the estimator in place
cal.export("calibration.json")  # ship to production
```

### Test-order helper

When you only have your own orders to learn from:

```python
helper = cal.test_order_helper(symbol=1, size=0.001)
helper.place(order_id=1001, price=50000.0,
             predicted_ahead=12.5, submit_ns=now_ns)
# ... wait for fill ...
helper.record_outcome(filled_at_ns=fill_ns, fill_qty=0.001,
                       observed_ahead_at_fill=10.0)
```

The helper writes the sample back into the parent calibrator
automatically.

## How the fit works

The toolkit does a grid search over `(half_life_ns, shrink_factor)`
on the configured grid and picks the pair that minimises the RMS
residual between adjusted estimator value and ground truth:

```
adjusted = estimator_value
         * exp(-elapsed_ns / half_life_ns)
         * shrink_factor ** shrink_events
residual = adjusted - ground_truth
```

Default grids cover 10s..10m half-life and 0.50..0.99 shrink
factor. Pass `half_life_grid_ns=[...]` / `shrink_grid=[...]` to
`fit()` to refine around a found minimum.

## Fit methods

`fit(method=...)` picks the optimiser. Choose based on the
trade-off between dependencies and precision:

| method          | dependencies | result quality                                | when to use                       |
|-----------------|--------------|-----------------------------------------------|-----------------------------------|
| `grid` (default)| none         | lands on grid point; RMSE ≈ grid spacing      | quick first pass, no extra deps  |
| `scipy`         | scipy        | continuous; RMSE ≤ grid (bootstrapped from grid)| precise off-grid fits           |
| `analytical`    | none         | closed-form when every sample shares `shrink_events`; exact on noiseless data | stationary scenarios with uniform shrink |

```python
result = cal.fit(method="grid")        # default
result = cal.fit(method="scipy")       # continuous; raises clear error if scipy missing
result = cal.fit(method="analytical")  # closed form; raises if shrink_events varies
```

`scipy` is imported lazily — the toolkit stays dependency-free for
the default path. `analytical` raises with guidance pointing you to
`grid` or `scipy` when its preconditions don't hold.

## Export / import

`CalibrationResult` round-trips as JSON via `to_json` /
`from_dict`. The toolkit's `export(path)` writes the latest fit
straight to disk; `LiveQueueCalibrator.load(path)` parses it back.

## Out of scope

- Real venue API integration (per-venue, user-credential-dependent)
- Continuous online calibration
- Cross-symbol calibration transfer
