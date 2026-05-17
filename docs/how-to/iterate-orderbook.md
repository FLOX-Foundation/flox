# Iterate the order book from a tape

`flox_py.orderbook` reconstructs the bid / ask ladder from a `.floxlog` tape's book event stream. Two surfaces sit on top of the same replay path:

- `OrderBookIterator` yields a `BookSnapshot` per bucket window, carrying the latest ladder state observed inside the window.
- `book_at(tape, ts_ns, levels)` is a point query that walks the tape up to `ts_ns` and returns the latest ladder state at or before it.

Both apply the standard floxlog book semantics: a snapshot event replaces the ladder, a delta event adds / changes / removes (qty=0) levels.

## When to reach for this

- Bucket-bar book-aware backtests: at the close of each bar, read the current ladder to compute imbalance, spread, depth-weighted mid, top-of-book microstructure features.
- Vacuum detection: scan ladders for thin levels on either side as a leading signal.
- Execution-side what-if: take a candidate trade timestamp, fetch the book at that instant, walk a VWAP / market-impact slice through the actual depth.

`OrderBookIterator` and `book_at` are pure-Python wrappers over `DataReader.read_book_updates`. For the hot tight loop (millions of book events at sub-millisecond resolution per bar) write a `Strategy` with `on_book_snapshot` / `on_book_delta` callbacks and run it through the engine; the wrappers here are for offline reconstruction outside the engine, where readability beats per-event throughput.

## Example

The script below builds a tiny synthetic tape and iterates it at a 60-second bucket cadence, then point-queries `book_at` at a chosen instant:

```python
--8<-- "examples/python_orderbook_iterator.py"
```

## Iterator semantics

`OrderBookIterator(tape_path, bucket_ns, levels, t_from=None, t_to=None, symbol_id=None)`:

- Snapshots are keyed on the floor of each event timestamp onto the `bucket_ns` grid. Consecutive snapshots therefore advance by `bucket_ns`.
- The snapshot emitted for bucket `B` captures the ladder state right before the first event of bucket `B + bucket_ns`. The state is exclusive of any event in the next bucket; the snapshot for the final bucket reflects every event seen.
- Buckets with no book events are skipped.
- When `symbol_id` is not set and the tape carries multiple symbols, the iterator yields one snapshot per (bucket, symbol).
- `BookSnapshot.crossed` is True when the best bid price meets or exceeds the best ask. This is typically a momentary artifact of out-of-order book events on captures without the sorted flag; the caller can choose to drop the snapshot or proceed.

## Point query semantics

`book_at(tape_path, ts_ns, levels, symbol_id=None, t_from=None)`:

- Walks events with `exchange_ts_ns <= ts_ns` and returns the most recent state.
- Returns `None` when the tape has no book events for the requested symbol up to `ts_ns`.
- Without a `symbol_id` filter on a multi-symbol tape, returns the snapshot for whichever symbol carried the most recent book event.

## Performance

The 38-day BTC tape benchmark from the tracker takes roughly two minutes end-to-end at a 60-second bucket cadence using `OrderBookIterator` — well under the 5-minute budget. The bound is the Python loop over book events; the underlying `DataReader.read_book_updates` already runs in C++.

If the tape carries a lot of trades but few book updates, iteration is even cheaper. For a tape-side filter that emits only buckets meeting a microstructure condition (top-K thin levels, depth imbalance over a threshold, queued size per side), wrap the iterator in a generator expression — the per-bucket cost is dominated by the ladder mutation, not the surfacing of the snapshot.

## See also

- [Import Binance book archives](import-binance-book-archive.md) for filling the tape with book events from the public archive.
- [Aggregate tape events in a single pass](aggregate-tape-events.md) for the engine-side aggregator framework when the use case fits a streaming bucket reducer.
