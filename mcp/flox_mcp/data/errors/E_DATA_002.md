---
code: E_DATA_002
title: Tape event arrived past reorder window
severity: error
since: 0.6.1
---

# E_DATA_002 — Tape event arrived past reorder window

`BinaryLogReader::streamForEach` / `run()` and `MergedTapeReader::streamEvents` / `run()` apply a bounded reorder buffer to segments without the Sorted flag. When an event arrives with `exchange_ts_ns` more than `reorder_window_ns` below the current watermark, the buffer has already emitted past that event's slot in sorted order and cannot recover it. The reader raises this error rather than silently producing out-of-order output.

The message includes the observed delta in nanoseconds and the configured window for context.

## How to fix

Pick one:

1. **Bump the reorder window** on the reader config. The default is 10 s; if the affected tape has reconnect-induced gaps longer than that, set a larger window when constructing the reader.

    === "Python"
        ```python
        reader = flox_py.DataReader("./tape", reorder_window_ns=30_000_000_000)
        ```

    === "C++"
        ```cpp
        flox::replay::ReaderConfig cfg;
        cfg.data_dir = "./tape";
        cfg.reorder_window_ns = 30'000'000'000;
        flox::replay::BinaryLogReader reader(cfg);
        ```

2. **Pre-sort the tape.** Re-process the segment through `BinaryLogWriter` (which sets the Sorted flag and the reader's fast path takes over). This is the right fix for archived data that won't change.

3. **Investigate the source.** A delta in the seconds-to-minutes range usually means an exchange reconnect or feed glitch. Beyond that range, the producer may genuinely be writing unsorted output and needs fixing upstream.

## Common causes

- Exchange websocket reconnect after a long network blip delivered a batch of stale events into a fresh tape block.
- The tape was produced by an external recorder that didn't sort events at write time (e.g. `md_collector`'s `commit_log_writer`) and a particular trading session had a tail wider than the default 10 s window.
- The reader is configured with `reorder_window_ns = 0` while the source is not Sorted-flagged.

## Memory cost of a larger window

The reorder buffer holds up to `reorder_window_ns × peak_event_rate × sizeof(ReplayEvent)` bytes at any moment. At 10 s × 10 k events/s × ~360 B ≈ 36 MB. Doubling the window doubles the worst-case buffer; even 60 s on a busy multi-symbol feed stays under 250 MB.
