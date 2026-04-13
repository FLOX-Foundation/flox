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

Read trades from binary log files.

```python
reader = flox.DataReader(
    data_dir="./data",
    from_ns=None,     # start timestamp filter
    to_ns=None,       # end timestamp filter
    symbols=None,     # list of symbol IDs to filter
)
```

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

## DataRecorder

High-level market data recorder with symbol metadata support.

```python
recorder = flox.DataRecorder(
    output_dir="./recordings",
    exchange_name="binance",
    max_segment_mb=256,
)
```

### Methods

#### `add_symbol(symbol_id, name, base="", quote="", price_precision=8, qty_precision=8)`

Register a symbol with metadata.

```python
recorder.add_symbol(1, "BTCUSDT", base="BTC", quote="USDT",
                     price_precision=2, qty_precision=6)
```

#### `start()`

Start recording.

#### `stop()`

Stop recording and finalize output.

#### `flush()`

Flush buffered data.

#### `is_recording() -> bool`

Check if currently recording.

#### `stats() -> dict`

Recorder statistics.

| Key | Type | Description |
|-----|------|-------------|
| `trades_written` | `int` | Trades recorded |
| `book_updates_written` | `int` | Book updates recorded |
| `bytes_written` | `int` | Bytes written |
| `files_created` | `int` | Files created |
| `errors` | `int` | Error count |
