---
code: E_BACKTEST_001
title: Bar type unsupported by writer
severity: error
since: 0.5.7
---

# E_BACKTEST_001 — Bar type unsupported by writer

`MmapBarWriter` only supports time-based bars. A non-time bar event
(tick / volume / range / Renko / Heikin-Ashi) was passed.

## How to fix

Pre-aggregate to a time bar before writing:

=== "Python"
    ```python
    # ✗ Won't work — tick bars can't be mmapped (variable-duration).
    # writer = flox.MmapBarWriter("out/BTCUSDT")
    # writer.on_bar(tick_bar)

    # ✓ Aggregate to time first, then write.
    time_bars = flox.aggregate_time_bars(
        timestamps=ts, prices=px, quantities=qty, is_buy=is_buy,
        interval_seconds=60,
    )
    for bar in time_bars:
        writer.on_bar(bar)
    ```

## Why

The mmap format relies on a fixed bar duration to compute file offsets
in O(1). Variable-duration bars (tick/volume/range/Renko) need a
different storage layout — open an issue if you need that.
