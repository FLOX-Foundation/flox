---
code: E_BACKTEST_001
title: Bar type unsupported by writer
severity: error
since: 0.5.7
---

# E_BACKTEST_001 — Bar type unsupported by writer

`MmapBarWriter` only supports time-based bars. A non-time bar
(tick / volume / range / Renko / Heikin-Ashi) reached `flush()` or
`writeBars()`, which cannot name a file for it.

`MmapBarWriter` is C++-only — it is not bound in Python, Node.js, Codon,
or QuickJS.

## How to fix

Give the writer only time timeframes. It subscribes to the bar bus, so
every interval the aggregator is configured with lands in its buffers —
a tick or volume interval blows up on the next `flush()`, not at
subscribe time:

```cpp
flox::MultiTimeframeAggregator<4> aggregator(&bus);
aggregator.addTimeInterval(std::chrono::seconds(60));    // written as bars_60s.bin
// aggregator.addTickInterval(1000);                      // ✗ not mmappable
// aggregator.addVolumeInterval(50.0);                    // ✗ not mmappable

flox::MmapBarWriter writer("/data/bybit/BTCUSDT/bars");
bus.subscribe(&writer);
```

Writing a batch directly carries the same constraint — the `TimeframeId`
has to be a time one:

```cpp
writer.writeBars(flox::TimeframeId::time(std::chrono::seconds(60)), bars);

// TimeframeId::tick(1000), ::volume(n), ::range(n) and ::renko(n) all
// throw E_BACKTEST_001 here.
```

If you need a variable-duration bar type, keep it out of the writer and
aggregate it per run. The batch aggregators cover every type and are the
only bar path in the bindings anyway: `flox.aggregate_tick_bars`,
`flox.aggregate_volume_bars`, `flox.aggregate_range_bars`,
`flox.aggregate_renko_bars`, `flox.aggregate_heikin_ashi_bars` (Node:
`aggregateTickBars`, ...). Their output goes to `run_bars` / `runBars`
with no bar file in between — see
[Bar aggregation](../how-to/bar-aggregation.md).

## Why

The mmap format relies on a fixed bar duration to compute file offsets
in O(1), and the filename itself encodes that duration (`bars_60s.bin`).
Variable-duration bars (tick/volume/range/Renko) need a different
storage layout — open an issue if you need that.
