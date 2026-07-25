# Iterate the order book from a tape

`flox_py.orderbook` reconstructs the bid / ask ladder from a `.floxlog` tape's book event stream. Two surfaces sit on top of the same replay path:

- `OrderBookIterator` yields a `BookSnapshot` per bucket window, carrying the latest ladder state observed inside the window.
- `book_at(tape_path, ts_ns=..., levels=...)` is a point query that walks the tape up to `ts_ns` and returns the latest ladder state at or before it.

Both apply the standard floxlog book semantics: a snapshot event replaces the ladder, a delta event adds / changes / removes (qty=0) levels.

## When to reach for this

- Bucket-bar book-aware backtests: at the close of each bar, read the current ladder to compute imbalance, spread, depth-weighted mid, top-of-book microstructure features.
- Vacuum detection: scan ladders for thin levels on either side as a leading signal.
- Execution-side what-if: take a candidate trade timestamp, fetch the book at that instant, walk a VWAP / market-impact slice through the actual depth.

`OrderBookIterator` and `book_at` are pure-Python wrappers over `DataReader.read_book_updates`. For the hot tight loop (millions of book events at sub-millisecond resolution per bar) write a `Strategy` with an `on_book_update` hook and run it through the engine; the wrappers here are for offline reconstruction outside the engine, where readability beats per-event throughput.

## Example

The script below builds a tiny synthetic tape and iterates it at a 60-second bucket cadence, then point-queries `book_at` at a chosen instant:

```python
--8<-- "examples/python_orderbook_iterator.py"
```

## Iterator semantics

```
OrderBookIterator(tape_path, *, bucket_ns, levels=20, t_from=None, t_to=None,
                  symbol_id=None, reorder_window_ns=None, reader_kwargs=None)
```

Everything after `tape_path` is keyword-only; `bucket_ns` is required.

- Snapshots are keyed on the floor of each event timestamp onto the `bucket_ns` grid. Consecutive snapshots therefore advance by `bucket_ns`.
- The snapshot emitted for bucket `B` captures the ladder state right before the first event of bucket `B + bucket_ns`. The state is exclusive of any event in the next bucket; the snapshot for the final bucket reflects every event seen.
- Buckets with no book events are skipped.
- When `symbol_id` is not set and the tape carries multiple symbols, the iterator yields one snapshot per (bucket, symbol).
- `levels` caps the number of levels surfaced per side; pass `0` to surface the full reconstructed ladder. Events deeper than `levels` are still applied — only the surfaced view is trimmed.
- `reorder_window_ns` is forwarded to the internal `DataReader`. It is not needed for correctness here: the wrapper stable-sorts the event headers by `exchange_ts_ns` before applying them, so arrival-order inversions on unsorted captures are already handled. The knob exists for parity with reader paths that enforce the window.
- `reader_kwargs` is forwarded verbatim to the internal `DataReader`. Time bounds and `reorder_window_ns` set explicitly win over entries here.
- `BookSnapshot.crossed` is True when the best bid price meets or exceeds the best ask. Because events are sorted before application, this reflects genuine venue-side crossing rather than capture-order artifacts; the caller can choose to drop the snapshot or proceed.

## Point query semantics

```
book_at(tape_path, *, ts_ns, levels=20, symbol_id=None, t_from=None,
        reorder_window_ns=None, reader_kwargs=None)
```

Everything after `tape_path` is keyword-only; `ts_ns` is required.

- Walks events with `exchange_ts_ns <= ts_ns` and returns the most recent state.
- Returns `None` when the tape has no book events for the requested symbol up to `ts_ns`.
- Without a `symbol_id` filter on a multi-symbol tape, returns the snapshot for whichever symbol carried the most recent book event.

## Performance

The 38-day BTC tape benchmark from the tracker takes roughly two minutes end-to-end at a 60-second bucket cadence using `OrderBookIterator` — well under the 5-minute budget. The bound is the Python loop over book events; the underlying `DataReader.read_book_updates` already runs in C++.

If the tape carries a lot of trades but few book updates, iteration is even cheaper. For a tape-side filter that emits only buckets meeting a microstructure condition (top-K thin levels, depth imbalance over a threshold, queued size per side), wrap the iterator in a generator expression — the per-bucket cost is dominated by the ladder mutation, not the surfacing of the snapshot.

## See also

- [Import Binance book archives](import-binance-book-archive.md) for filling the tape with book events from the public archive.
- [Aggregate tape events in a single pass](aggregate-tape-events.md) for the engine-side aggregator framework when the use case fits a streaming bucket reducer. `flox_py.BookSnapshotBinAggregator` is the C++ fast path for exactly this bucket-and-ladder shape — same bucket semantics, run it with `n_threads=1`. Prefer it for month-scale multi-symbol extraction; the wrappers here are for point queries and hours-scale windows.
