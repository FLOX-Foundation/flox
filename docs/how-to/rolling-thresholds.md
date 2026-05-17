# Rolling top-K thresholds for extreme-event filters

A common pattern in research strategies: "fire when this bar's metric is in the top-K of the trailing N-bar window." Range breakout, volume spike, abs-return tail, count-of-prints all reduce to the same primitive once the per-bar metric is computed.

The naive Python loop is fine at hourly resolution:

```text
WARMUP = 30 * 24   # 30 days of 1h bars
for i in range(WARMUP, n):
    win = metric[i - WARMUP : i]
    thr = np.partition(win, -K)[-K]
    if metric[i] >= thr:
        ...
```

At 17,000 bars it runs in ~7 seconds. At 15-minute resolution (70k bars) it crosses a minute. At 1-second resolution (60M bars) it crosses an hour. `flox_py.rolling.top_k_threshold` collapses the same computation to a single vectorized `np.partition` call along the window axis.

## Function

```text
flox_py.rolling.top_k_threshold(
    values: ArrayLike,
    *,
    window: int,
    k: int = 1,
    out_dtype: np.dtype | None = None,
) -> np.ndarray
```

`thr[i]` is the `k`-th largest value in `values[i - window : i]`. The bar at index `i` itself is excluded, so the helper does not leak future information into the threshold. The first `window` slots are filled with `NaN` so callers can mask out the warmup with `~np.isnan(thr)`.

## Example

The script below is the same one CI runs on every push. It computes a top-3 threshold over a 100-bar trailing window on a synthetic per-bar metric and confirms the result matches an explicit `np.partition` call:

```python
--8<-- "examples/python_rolling_top_k.py"
```

## Performance

Benchmark on 17,520 bars (BTC 1h × 2 years), `window=720`, `k=3`:

| Implementation | Time |
|---|---|
| Naive Python loop with `np.partition` per bar | ~7 s |
| `flox_py.rolling.top_k_threshold` | ~25 ms |

At 1-minute (or finer) resolution the difference goes from "the script runs" to "the script does not run." The helper makes the lower timeframes accessible without writing per-call boilerplate around `np.lib.stride_tricks.sliding_window_view`.

## When to use a C++ aggregator instead

If the per-bar metric is itself derived from a tape (not from a pre-computed numpy array), pair `top_k_threshold` with the `OHLCBinAggregator` so the metric and the threshold are produced in a single pass over the tape. The threshold computation stays in numpy because the work is bounded by the OHLC output (one row per bar, not one row per trade); the savings from pushing it down to C++ would not be measurable next to the I/O.

A streaming per-trade top-K (a tape-side filter that emits only the buckets above some trailing-window threshold, without materialising every bucket) is tracked as a separate aggregator task when a use case actually needs it.
