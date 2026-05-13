# Replay

Read, write, and record market data in Flox binary format. Supports LZ4 compression, time-range filtering, and symbol filtering.

## inspect()

Quick summary of a data directory without loading all data.

```python
info = flox.inspect(data_dir="./data")
print(f"Events: {info['total_events']}, Duration: {info['duration_seconds']:.0f}s")
```

**Returns dict:**

| Key | Type | Description |
|-----|------|-------------|
| `first_event_ns` | `int` | First event timestamp |
| `last_event_ns` | `int` | Last event timestamp |
| `total_events` | `int` | Total event count |
| `segment_count` | `int` | Number of segment files |
| `total_bytes` | `int` | Total data size |
| `duration_seconds` | `float` | Time span in seconds |
| `fully_indexed` | `bool` | Whether all segments have indexes |

---

## DataReader

Read trades and book updates from binary log files.

```python
reader = flox.DataReader(
    data_dir="./data",
    from_ns=None,             # start timestamp filter
    to_ns=None,               # end timestamp filter
    symbols=None,             # list of symbol IDs to filter
    reorder_window_ns=None,   # cross-block reorder window (default 10s)
)
```

`reorder_window_ns` controls the bounded reorder buffer used by `run()` and `streamForEach` on segments without the `Sorted` flag. Events arriving more than `reorder_window_ns` past the watermark raise `FloxError(code="E_DATA_002")`. See [Aggregate tape events](../../how-to/aggregate-tape-events.md#out-of-order-tapes-and-the-reorder-buffer) for the full discussion.

### Methods

#### `summary() -> dict`

Dataset summary (same keys as `inspect()` plus `data_dir` and `symbols`).

#### `count() -> int`

Total event count.

#### `symbols() -> set[int]`

Set of available symbol IDs.

#### `time_range() -> tuple[int, int] | None`

`(start_ns, end_ns)` tuple, or `None` if empty.

#### `read_trades() -> ndarray`

Read all trades as a numpy structured array with `PyTrade` dtype.

```python
trades = reader.read_trades()
prices = trades['price_raw'] / 1e8
quantities = trades['qty_raw'] / 1e8
```

#### `read_trades_from(start_ts_ns) -> ndarray`

Read trades starting from a given timestamp.

```python
trades = reader.read_trades_from(start_ts_ns=1704067200_000_000_000)
```

#### `read_bbo() -> ndarray`

Read top-of-book (best bid/ask) from every book update event as a numpy structured array with `PyBBO` dtype.

```python
bbos = reader.read_bbo()
mids = (bbos['bid_price_raw'] + bbos['ask_price_raw']) / 2 / 1e8
```

#### `read_bbo_from(start_ts_ns) -> ndarray`

Same as `read_bbo()` but starts iterating from the first event whose `exchange_ts_ns >= start_ts_ns`.

```python
bbos = reader.read_bbo_from(start_ts_ns=1704067200_000_000_000)
```

#### `read_book_updates() -> tuple[ndarray, ndarray]`

Read every book update event with full depth. Returns `(headers, levels)`:

- `headers` — structured array of `PyBookUpdateHeader`. Each row carries `level_offset`, `bid_count`, `ask_count` for slicing the levels array.
- `levels` — single flat structured array of `PyLevel` shared by all events; bids first, then asks, per event.

```python
headers, levels = reader.read_book_updates()
for h in headers:
    off, nb, na = h['level_offset'], h['bid_count'], h['ask_count']
    bids = levels[off : off + nb]
    asks = levels[off + nb : off + nb + na]
```

#### `read_book_updates_from(start_ts_ns) -> tuple[ndarray, ndarray]`

Same as `read_book_updates()` but starts iterating from the first event whose `exchange_ts_ns >= start_ts_ns`.

```python
headers, levels = reader.read_book_updates_from(start_ts_ns=1704067200_000_000_000)
```

#### `stats() -> dict`

Reader statistics.

| Key | Type | Description |
|-----|------|-------------|
| `files_read` | `int` | Segment files read |
| `events_read` | `int` | Total events processed |
| `trades_read` | `int` | Trade events |
| `book_updates_read` | `int` | Book update events |
| `bytes_read` | `int` | Bytes read |
| `crc_errors` | `int` | CRC checksum failures |

#### `run(aggregators, n_threads=0) -> bool`

Single-pass streaming aggregator dispatch. Walks the tape once and forwards each event to every aggregator's `onEvent`, then calls `finalize()` once on each. The GIL is released for the whole walk; aggregators must be self-contained.

```python
reader.run([stats, peaks, quantiles])           # n_threads=0 → auto
reader.run([stats, peaks, quantiles], 4)        # 4 workers
reader.run([stats, peaks, quantiles], 1)        # explicit single-thread
```

`n_threads` policy:

- `0` (default): auto, resolved to `min(blocks_per_segment / 2, hardware_concurrency())`.
- `1`: explicit single-thread.
- `>1`: explicit worker count, capped to the effective block count per segment.

Parallel mode partitions the segment at the compressed-block level — each worker holds a panel cloned via `cloneEmpty()` and walks its assigned block range; results merge via `merge()` before `finalize()`. See [Aggregate tape events](../../how-to/aggregate-tape-events.md#parallel-execution) for the boundary semantics on sliding-window aggregators.

#### `segment_files() -> list[str]`

List of segment file paths.

#### `segments() -> list[dict]`

Segment metadata.

| Key | Type | Description |
|-----|------|-------------|
| `path` | `str` | File path |
| `first_event_ns` | `int` | First event timestamp |
| `last_event_ns` | `int` | Last event timestamp |
| `event_count` | `int` | Events in segment |
| `has_index` | `bool` | Whether segment has an index |

### PyTrade Dtype

| Field | Type | Description |
|-------|------|-------------|
| `exchange_ts_ns` | `int64` | Exchange timestamp (ns) |
| `recv_ts_ns` | `int64` | Local receive timestamp (ns) |
| `price_raw` | `int64` | Price * 10^8 |
| `qty_raw` | `int64` | Quantity * 10^8 |
| `trade_id` | `uint64` | Exchange trade ID |
| `symbol_id` | `uint32` | Symbol ID |
| `side` | `uint8` | 0 = buy, 1 = sell |

### PyBBO Dtype

| Field | Type | Description |
|-------|------|-------------|
| `exchange_ts_ns` | `int64` | Exchange timestamp (ns) |
| `recv_ts_ns` | `int64` | Local receive timestamp (ns) |
| `seq` | `int64` | Exchange sequence number |
| `symbol_id` | `uint32` | Symbol ID |
| `event_type` | `uint8` | 2 = snapshot, 3 = delta |
| `bid_price_raw` | `int64` | Best bid price * 10^8 (0 if absent) |
| `bid_qty_raw` | `int64` | Best bid quantity * 10^8 |
| `ask_price_raw` | `int64` | Best ask price * 10^8 (0 if absent) |
| `ask_qty_raw` | `int64` | Best ask quantity * 10^8 |

### PyBookUpdateHeader Dtype

| Field | Type | Description |
|-------|------|-------------|
| `exchange_ts_ns` | `int64` | Exchange timestamp (ns) |
| `recv_ts_ns` | `int64` | Local receive timestamp (ns) |
| `seq` | `int64` | Exchange sequence number |
| `symbol_id` | `uint32` | Symbol ID |
| `bid_count` | `uint16` | Number of bid levels for this event |
| `ask_count` | `uint16` | Number of ask levels for this event |
| `level_offset` | `uint32` | Index of this event's first level in the levels array |
| `event_type` | `uint8` | 2 = snapshot, 3 = delta |

### PyLevel Dtype

| Field | Type | Description |
|-------|------|-------------|
| `price_raw` | `int64` | Price * 10^8 |
| `qty_raw` | `int64` | Quantity * 10^8 |
| `side` | `uint8` | 0 = bid, 1 = ask |

---

## DataWriter

Write trade data to binary log files.

```python
writer = flox.DataWriter(
    output_dir="./output",
    max_segment_mb=256,
    exchange_id=0,
    compression="none",   # "none" or "lz4"
)
```

### Methods

#### `write_trade(exchange_ts_ns, recv_ts_ns, price, qty, trade_id, symbol_id, side) -> bool`

Write a single trade. Returns `True` on success.

```python
writer.write_trade(
    exchange_ts_ns=1704067200_000_000_000,
    recv_ts_ns=1704067200_001_000_000,
    price=50000.0,
    qty=1.5,
    trade_id=12345,
    symbol_id=1,
    side=0,
)
```

#### `write_trades(exchange_ts_ns, recv_ts_ns, prices, quantities, trade_ids, symbol_ids, sides) -> int`

Vectorized write from numpy arrays. Returns number of trades written.

```python
count = writer.write_trades(
    exchange_ts_ns=ts_array,
    recv_ts_ns=recv_array,
    prices=price_array,
    quantities=qty_array,
    trade_ids=id_array,
    symbol_ids=sym_array,
    sides=side_array,
)
```

#### `flush()`

Flush buffered data to disk.

#### `close()`

Close the writer and finalize all segments.

#### `stats() -> dict`

Writer statistics.

| Key | Type | Description |
|-----|------|-------------|
| `bytes_written` | `int` | Total bytes |
| `events_written` | `int` | Total events |
| `segments_created` | `int` | Segment files created |
| `trades_written` | `int` | Trade events |
| `book_updates_written` | `int` | Book update events |
| `blocks_written` | `int` | Data blocks |
| `uncompressed_bytes` | `int` | Uncompressed size |
| `compressed_bytes` | `int` | Compressed size |

#### `current_segment_path() -> str`

Path of the current segment being written.

---

## BinaryLogRecorderHook

Built-in `.floxlog` recorder. Plug into a `Runner` via
`runner.set_market_data_recorder(hook)`. Lifecycle is driven by the
engine; both trades and book updates are captured.

```python
hook = flox.BinaryLogRecorderHook(
    "./recordings",
    max_segment_mb=256,
    exchange_id=0,
    compression="none",   # or "lz4"
)
hook.add_symbol(1, "BTCUSDT", base="BTC", quote="USDT",
                price_precision=2, qty_precision=6)
runner.set_market_data_recorder(hook)
```

### Methods

#### `add_symbol(symbol_id, name, base="", quote="", price_precision=8, qty_precision=8)`

Register a symbol in the recording metadata.

#### `flush()`

Flush buffered bytes to disk.

#### `close()`

Stop the underlying writer (idempotent — also called by the engine's
on-stop lifecycle).

#### `current_segment_path() -> str`

Path of the segment currently being written. Empty before `start()`.

#### `stats() -> dict`

| Key | Type | Description |
|-----|------|-------------|
| `trades_written` | `int` | Trades recorded |
| `book_updates_written` | `int` | Book updates recorded |
| `bytes_written` | `int` | Bytes written |
| `segments_created` | `int` | Segments written |
| `errors` | `int` | Writer rejections |
