# Import Binance public book archives into a floxlog tape

The aggTrades importer covered in [Import Binance public archives](import-binance-archive.md) handles the trade stream. The book side of the archive ships in two separate products on `data.binance.vision`:

- **bookTicker** (alias `t1`): best bid / ask snapshot per book update. Light, available for spot / um-futures / cm-futures.
- **bookDepth** (alias `depth20`): top-20 levels per side per book update. Heavier (around 10x aggTrades), today only published for um-futures.

Both products live alongside `aggtrades_to_floxlog` in `flox_py.archives.binance` and emit book events into the same `.floxlog` tape directory, so a single tape can carry trades + book updates the engine and `MergedTapeReader` read interleaved.

## Functions

```text
binance.t1_to_floxlog(csv_path, out_tape, *, symbol_id=1, symbol_name="",
                      market="um-futures", append=True, ...)
                      -> BookConvertStats

binance.depth20_to_floxlog(csv_path, out_tape, *, levels=20, ...)
                      -> BookConvertStats

binance.range_book_to_floxlog(
    symbol, market, book_type, date_from, date_to, out_tape, *,
    mirror=None, parallel=2, levels=20, ...,
) -> BookConvertStats
```

`book_type` is `"t1"` or `"depth20"`. The range form downloads missing zips from `data.binance.vision`, caches them under `mirror` when set, and runs the converter on the local files serially so the writer stays append-safe.

## Example

The script below is the same one CI runs on every push. It builds synthetic bookTicker and bookDepth zips, converts each to a separate `.floxlog`, and reads the book events back through `DataReader.read_book_updates`:

```python
--8<-- "examples/python_binance_book_archive.py"
```

## Delta encoding

bookTicker rows are best-bid / best-ask snapshots, one per update. The first imported row becomes a book snapshot; every subsequent row emits a delta with the changed top level (qty=0 marks a removed price). Unchanged ticks are skipped so the tape does not accumulate dead data.

bookDepth is published in long format: one row per (event, side, price) tuple, grouped by `last_update_id`. The first group with `update_type=snap` becomes a snapshot containing every (price, qty) level for both sides; later groups emit a delta with only the levels that changed against the prior snapshot, again with qty=0 marking removals.

Both converters keep the running ladder state in memory while writing, so the engine's existing book replay (snapshot replaces book, delta applies on top) gives the same `(bids, asks)` sequence the original CSV described.

## CLI

`flox archive binance` accepts the same `--book` flag on either form:

```bash
# Single day from a local CSV
flox archive binance --book t1 \
  --csv ./BTCUSDT-bookTicker-2024-01-15.zip \
  --out ./tapes/binance-um-BTCUSDT \
  --symbol BTCUSDT --market um-futures

# Multi-day range with download
flox archive binance --book depth20 \
  --symbol BTCUSDT --market um-futures \
  --from 2024-01-01 --to 2024-01-07 \
  --out ./tapes/binance-um-BTCUSDT \
  --mirror ./.cache/binance \
  --parallel 2
```

Use `--book-levels` to cap the ladder width if the archive publishes more than the default 20 levels per side.

## Append-safe and idempotent

The converter dedups on the `update_id` (bookTicker) or `last_update_id` (bookDepth) it has already written to the tape. Re-running the same day, or running an overlapping range, is a no-op: every previously-imported row is skipped, and the writer adds zero new events. The reported `rows_skipped` counter shows how many rows were elided.

## Co-existence with aggTrades

Trades and book events sit in one `.floxlog` directory side by side. The engine and `MergedTapeReader` interleave them on `exchange_ts_ns` at read time, so the same tape feeds both trade aggregators and book-level analyses (depth-aware execution, vacuum / queue analysis, top-of-book microstructure) in a single pass.

`metadata.json` carries the union counters: `total_trades`, `total_book_updates`, `has_trades`, `has_book_snapshots`, `has_book_deltas`, and the maximum observed `book_depth`. The MergedTapeReader keys by `(metadata.exchange, name)` exactly as for the trade-only case.

## See also

- [Import Binance public archives](import-binance-archive.md) for the aggTrades counterpart.
- [Merge multiple tapes on read](multi-tape.md) for combining the imported book + trade tape with tapes from other exchanges.
