# Aggregate tape events in a single pass

`DataReader.run([...])` walks a `.floxlog` once and dispatches every event to a panel of streaming aggregators. The aggregators do their work in C++ (no per-event Python or JS crossings), so a panel of five aggregators costs one decompression pass, not five.

The five native aggregators:

| Aggregator | Per-cell value | Typical use |
|---|---|---|
| `EventTypeStatsAggregator` | Counter of trades / book snapshots / book deltas | "What's in this tape" overview |
| `BinCountAggregator` | Event count per time bucket | "Events per minute / per second / per bar" |
| `VolumeBinAggregator` | Sum of trade `qty_raw` per time bucket | Volume profile over time |
| `PeakAggregator` | Top-N busiest fixed-width windows per scale | "When were the bursts" |
| `QuantileAggregator` | Window-count distribution → quantiles | Baseline activity at a given scale |

All five accept the same two filter parameters:

- `event_filter`: `Trades` / `BooksOnly` / `Both`. Aggregators that only make sense for trades (volume, peaks, quantiles) default to `Trades`.
- `symbol_filter`: list of symbol ids. Empty (the default) admits every symbol.

Pass `run([])` to verify the framework is wired up. It's a no-op and does not decompress anything.

## Python

```python
import flox_py

reader = flox_py.DataReader("./tape")

stats = flox_py.EventTypeStatsAggregator()
counts = flox_py.BinCountAggregator(
    bucket_ns=60_000_000_000,  # 1-minute buckets
    by_side=True,
)
volume = flox_py.VolumeBinAggregator(
    bucket_ns=60_000_000_000,
    by_side=True,
)
peaks = flox_py.PeakAggregator(
    window_ns_list=[1_000_000, 10_000_000, 100_000_000, 1_000_000_000],
    top_n=10,
)
quantiles = flox_py.QuantileAggregator(
    window_ns_list=[1_000_000, 100_000_000],
    quantiles=[0.5, 0.95, 0.99],
)

reader.run([stats, counts, volume, peaks, quantiles])

# All five .result() accessors return their final shape after run() finishes.
print("symbol summary:", stats.result())          # structured numpy array
print("trades per minute:", counts.result())      # structured numpy array
print("volume per minute:", volume.result())      # structured numpy array
print("busiest windows:", peaks.result())         # dict[window_ns, list[(count, start_ns)]]
print("activity baseline:", quantiles.result())   # dict[window_ns, dict[quantile, count]]
```

The tabular aggregators (`stats`, `counts`, `volume`) return structured numpy arrays. Field names match the row struct: `arr["count"]`, `arr["bucket_ts_ns"]`, `arr["qty_raw"]`, and so on.

`side` encoding in `BinCount` / `VolumeBin` rows is `0 = aggregate, 1 = BUY, 2 = SELL`. `symbol_id = 0` means "aggregate" (the `by_symbol=False` case). Pass `by_symbol=True` for one row per symbol per bucket.

## Node.js

```js
import { DataReader, EventTypeStatsAggregator, BinCountAggregator,
         VolumeBinAggregator, PeakAggregator, QuantileAggregator,
         AggregatorEventFilter } from '@flox-foundation/flox';

const reader = new DataReader('./tape');

const F = AggregatorEventFilter;
const stats = new EventTypeStatsAggregator(F.Both, []);
const counts = new BinCountAggregator(60_000_000_000n, /*bySide=*/true);
const volume = new VolumeBinAggregator(60_000_000_000n, /*bySide=*/true);
const peaks = new PeakAggregator(
    [1_000_000n, 10_000_000n, 100_000_000n, 1_000_000_000n],
    /*topN=*/10
);
const quantiles = new QuantileAggregator(
    [1_000_000n, 100_000_000n],
    [0.5, 0.95, 0.99]
);

reader.run([stats, counts, volume, peaks, quantiles]);

console.log('symbol summary:', stats.result());
console.log('trades per minute:', counts.result());
console.log('volume per minute:', volume.result());
console.log('busiest windows:', peaks.result());
console.log('activity baseline:', quantiles.result());
```

Node returns BigInt for ns timestamps and 64-bit counters, so values past `2^53` keep precision. The `Peak` result is `Array<{windowNs, peaks: Array<{count, startNs}>}>` (not a `Map`); iterate it with `for (const entry of result)`.

## Merged tapes

`MergedTapeReader.run([...])` accepts the same aggregator panel and walks the merged stream from N input tapes in a single pass. Aggregators see events with global-rewritten symbol ids; per-tape provenance is not surfaced. Use `streamEvents()` directly when the tape index matters.

```python
merged = flox_py.MergedTapeReader(["./tape-bybit", "./tape-bitget"])
peaks = flox_py.PeakAggregator(window_ns_list=[1_000_000_000], top_n=20)
merged.run([peaks])
```

## Aggregator semantics

### EventTypeStats

Result is sorted by `symbol_id` ascending. Symbols with zero matching events do not appear. Pass an explicit `symbol_filter` when you need a stable shape across runs.

### BinCount / VolumeBin

The bucket for an event at `t` is `floor(t / bucket_ns) * bucket_ns`. Empty buckets produce no rows. `by_side=True` only splits trades (book events stay in `side=0`); `by_symbol=True` produces one row per (bucket, symbol_id) cell.

`VolumeBin` ignores book events entirely. There is no scalar `qty` per book event that maps cleanly to a volume bucket, so `event_filter=BooksOnly` yields an empty result by design.

### Peak

For each `window_ns`, finds the top-N intervals of width `window_ns` containing the most events. Adjacent peaks within `3 × window_ns` of a stronger one are suppressed at `finalize()` so distinct bursts dominate the result instead of the neighbours of one big burst.

`oversample_factor` (default `100`) controls the in-flight candidate heap size as `top_n × oversample_factor`. Raise it on tapes with many distinct bursts that cluster within `3 × window_ns` of an earlier candidate.

`start_ns` in the result is `t - window_ns`, where `t` is the arrival time of the last event in the window. The window starts `window_ns` earlier and ends at the event.

### Quantile

For each `window_ns`, the aggregator records the sliding-window event count at every arrival, builds a histogram of those observations, and at `finalize()` resolves each quantile `q` to the smallest count value `C` such that `fraction(observed ≤ C) ≥ q`.

Quantiles must be in `(0.0, 1.0]`. The result row count is `len(window_ns_list) × len(quantiles)`. When `event_filter` filters everything out, the rows still appear with `count=0` so the shape stays stable.

## Performance notes

- Each aggregator's `onEvent` is called once per event per pass. The dispatch loop in C++ adds one indirection per event per aggregator. Bigger panels cost more per event, not more passes.
- All five aggregators are self-contained: no I/O, no Python crossings, no hot-path allocations beyond the bounded sliding deques in `Peak` / `Quantile`.
- For multi-tape captures, the C++ N-way heap merge in `MergedTapeReader` is the right entry point. `run()` walks the merged stream in `O(N_tapes)` peak memory regardless of total event count.
- `Quantile` stores its histogram as `unordered_map<count_value, occurrences>`. Distinct count values are bounded by the peak burst size (typically dozens) while observations are huge (one per event per scale), so memory stays in the kilobytes even on multi-GB captures.

## See also

- [Record and replay market data with `flox tape`](tape-record.md) for capturing the `.floxlog`.
- [Merged tape consumption](multi-tape-replay.md) for cases where streaming `run()` is not enough.
