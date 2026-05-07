# Execution algorithms (TWAP / VWAP / Iceberg / POV)

A strategy emits a target intent ("buy 1.0 BTC over the next hour" or "sell into 5 percent of market volume"). The execution algorithm turns that intent into a stream of child orders. flox ships four composable algos that speak the same minimal `submit_order` contract; pass any object that implements that signature and the same code runs in backtest and in live.

These are Python-side primitives. The C++ engine has the same `ExecutionListener` hook surface, so cross-binding parity for the algo classes themselves is a Phase 2 follow-up.

## When to reach for which algo

| Algo | Use when |
|---|---|
| `TWAPExecutor` | You want to spread an order evenly in time. Equal-time slicing across a fixed duration. The default for "I have an hour, slice it into 12 child orders". |
| `VWAPExecutor` | You have a volume curve (historical or live forecast) and want slice sizes proportional to expected volume. Better than TWAP when intraday volume is uneven. |
| `IcebergExecutor` | You want to hide size. Show only `visible_qty`; resubmit the next slice when the previous one fills. |
| `POVExecutor` | You want to participate at a fixed share of observed market volume. Adapts to live conditions; you feed it `observe_volume(qty)` from your trade tick. |

All four track `target_qty`, `submitted_qty`, `filled_qty`, and `remaining_qty`. None of them place duplicate orders or exceed the target. None of them know anything specific about your engine; they call `submit_order` and ask you to feed them fills (and, for POV, observed volume).

## Quick start

```python
import flox_py as flox
from flox_py.execution_algos import TWAPExecutor

sim = flox.SimulatedExecutor()

twap = TWAPExecutor(
    target_qty=10.0,
    side="buy",
    symbol=1,
    duration_ns=3_600_000_000_000,  # 1 hour
    slice_count=12,
    start_time_ns=now_ns,
)

# In your tick / timer loop:
def on_tick(now_ns):
    twap.step(now_ns, sim)
    if twap.is_done():
        print(f"submitted {twap.submitted_qty}, filled {twap.filled_qty}")

# When the simulator (or your live broker) reports a fill, feed it back:
def on_fill(qty):
    twap.report_fill(qty)
```

The same pattern works for the other three algos. The only differences are the constructor arguments and what each one needs to know to make its decisions.

## VWAP volume curve

`VWAPExecutor` takes `volume_curve` as a sequence of `(bar_ts_ns, volume)` pairs ordered by timestamp. Each bar's slice is `(bar_volume / total_volume) * target_qty`. Bars with zero volume are skipped.

```python
from flox_py.execution_algos import VWAPExecutor

curve = [
    (bar_ts_ns(9, 30), 1_000.0),
    (bar_ts_ns(9, 31), 1_500.0),
    (bar_ts_ns(9, 32), 2_000.0),
    # ... full intraday profile
]
vwap = VWAPExecutor(
    target_qty=100.0, side="buy", symbol=1,
    volume_curve=curve,
)
```

Use a historical 5-day average curve as a starting point. If you have a live volume forecaster, feed its output into the curve; the algo recomputes shares based on whatever you pass.

## Iceberg fill loop

`IcebergExecutor` only emits a new child when the previous one is fully filled. The user app must call `report_fill(qty)` so the algo knows when to step again.

```python
from flox_py.execution_algos import IcebergExecutor

ice = IcebergExecutor(
    target_qty=100.0, side="sell", symbol=1,
    visible_qty=10.0, type="limit", price=68_500.0,
)
ice.step(now_ns, sim)         # submits 10.0
# ... fill observed ...
ice.report_fill(10.0)
ice.step(now_ns, sim)         # submits the next 10.0
# ... and so on until target_qty is reached
```

The last slice is naturally smaller than `visible_qty` if the remainder does not divide evenly.

## POV market-volume tracking

`POVExecutor` chases a fixed share of observed market volume. You feed it volume, it decides how much to submit so that cumulative submitted is approximately `participation_rate * cumulative observed volume`.

```python
from flox_py.execution_algos import POVExecutor

pov = POVExecutor(
    target_qty=50.0, side="buy", symbol=1,
    participation_rate=0.05,    # 5 percent
    min_slice_qty=0.5,           # do not submit anything smaller
)

def on_trade(ts_ns, sym, price, qty, is_buy):
    pov.observe_volume(qty)
    pov.step(ts_ns, sim)
```

`min_slice_qty` is the floor below which the algo holds back; useful when round-tripping tiny orders is wasteful.

## What's not here yet

- `AdaptiveExecutor` that switches strategies based on market conditions. The composition is non-trivial and this layer is intentionally simple.
- C++ binding parity. The algo classes are Python-only today; Phase 2 will mirror them on the C++ `ExecutionListener` surface and through pybind11 / NAPI / Codon / QuickJS.
- Backtest-fidelity tuning. Slippage and queue-aware fills already work through `flox_py.SimulatedExecutor`. Latency-aware fill timing pairs naturally with the `latency_models` module from W6.

## See also

* [Backtest with realistic fills](backtest-realistic-fills.md). The slippage and queue knobs that make these algos backtest-faithful.
* [Backtest with latency](backtest-with-latency.md). Sample latencies you can apply between the algo's `submit_order` call and the simulator's matching tick.
* [Paper trading](paper-trading.md). The wrapper that drives an algo against a live data feed without touching real exchange credentials.
