# Recording Data

Capture live market data to disk for later replay and backtesting. The same `.floxlog` binary format is produced by every binding — record from Python or Node.js and replay from C++ later, or vice versa.

## Prerequisites

- Completed [First Strategy](first-strategy.md)
- Optional: LZ4 library for compression (`-DFLOX_ENABLE_LZ4=ON`)

## What gets recorded

FLOX writes a custom binary format optimised for sequential writes and fast sequential / indexed reads. Optional LZ4 compression. Automatic segment rotation.

## Basic recording

Two ways to write a `.floxlog`:

1. **Attach a `BinaryLogRecorderHook` to a `Runner`** (recommended). The runner feeds trades and book updates straight into the writer on the C++ side — no per-event managed-language callback.
2. **Use the low-level `DataWriter`** directly when you're synthesising data outside a runner (e.g. converting an arbitrary archive into `.floxlog`).

=== "Python — runner attach"

    ```python
    import flox_py as flox

    hook = flox.BinaryLogRecorderHook(
        "/data/btcusdt",
        max_segment_mb=256,
        exchange_id=0,
        compression="none",   # or "lz4"
    )
    hook.add_symbol(1, "BTCUSDT", base="BTC", quote="USDT",
                    price_precision=2, qty_precision=3)

    runner = flox.Runner(registry, on_signal=lambda _s: None)
    runner.set_market_data_recorder(hook)
    runner.start()
    # ... events flow through the runner ...
    runner.stop()
    print(hook.stats())
    ```

=== "Python — direct DataWriter"

    ```python
    import flox_py as flox
    import numpy as np

    w = flox.DataWriter("/data/btcusdt", max_segment_mb=256, exchange_id=0,
                        compression="none")
    w.write_trade(exchange_ts_ns=ts_ns, recv_ts_ns=ts_ns,
                  price=10050.0, qty=0.5, trade_id=0, symbol_id=1, side=0)

    bids = np.array([(1005000000000, 50000000, 0)],
                    dtype=[("price_raw","i8"),("qty_raw","i8"),("side","u1")])
    asks = np.array([(1005100000000, 30000000, 1)], dtype=bids.dtype)
    w.write_book(exchange_ts_ns=ts_ns, recv_ts_ns=ts_ns, seq=0,
                 symbol_id=1, is_snapshot=True, bids=bids, asks=asks)
    w.close()
    ```

=== "Node.js — runner attach"

    ```javascript
    const hook = new flox.BinaryLogRecorderHook(
      "/data/btcusdt", 256, 0, "none");
    hook.addSymbol(1n, "BTCUSDT", "BTC", "USDT", 2, 3);

    runner.setMarketDataRecorder(hook);
    runner.start();
    // ... events flow ...
    runner.stop();
    console.log(hook.stats());
    ```

=== "Node.js — direct DataWriter"

    ```javascript
    const w = new flox.DataWriter("/data/btcusdt", 256, 0);
    w.writeTrade(tsNs, tsNs, 10050.0, 0.5, 0n, 1, 0);
    // Book levels are flat BigInt64Array: [price_raw, qty_raw, ...]
    const bids = new BigInt64Array([1005000000000n, 50000000n]);
    const asks = new BigInt64Array([1005100000000n, 30000000n]);
    w.writeBook(tsNs, tsNs, 0n, 1, true, bids, asks);
    w.close();
    ```

=== "C++"

    ```cpp
    #include "flox/replay/writers/binary_log_writer.h"
    using namespace flox::replay;

    WriterConfig config;
    config.output_dir = "/data/market_data";
    config.max_segment_bytes = 256 * 1024 * 1024;
    config.create_index = true;
    config.compression = CompressionType::None;       // or LZ4

    BinaryLogWriter writer(config);

    TradeRecord trade{ .symbol_id = 42, .price = 10050, .quantity = 100,
                        .side = 1, .exchange_ts_ns = ts_ns };
    writer.writeTrade(trade);

    BookRecordHeader header{ .symbol_id = 42, .update_type = static_cast<uint8_t>(BookUpdateType::SNAPSHOT),
                              .bid_count = 5, .ask_count = 5, .exchange_ts_ns = ts_ns };
    writer.writeBook(header, bids, asks);

    writer.flush();
    writer.close();
    ```

## 3. Recording from Live Connectors

Subscribe the writer to your market data buses:

```cpp
// Bespoke subscriber that writes both buses into one .floxlog.
// Functionally equivalent to flox::replay::BinaryLogRecorderHook, but
// shown here as a worked example of the underlying writer surface.
class CustomBusRecorder : public IMarketDataSubscriber
{
public:
  CustomBusRecorder(const std::filesystem::path& output_dir)
    : _writer(createConfig(output_dir)) {}

  SubscriberId id() const override {
    return reinterpret_cast<SubscriberId>(this);
  }

  void onTrade(const TradeEvent& ev) override {
    TradeRecord record;
    record.symbol_id = ev.trade.symbol;
    record.price = ev.trade.price.raw();
    record.quantity = ev.trade.quantity.raw();
    record.side = ev.trade.isBuy ? 1 : 0;
    record.exchange_ts_ns = ev.trade.exchangeTsNs;

    _writer.writeTrade(record);
  }

  void onBookUpdate(const BookUpdateEvent& ev) override {
    BookRecordHeader header;
    header.symbol_id = ev.update.symbol;
    header.update_type = static_cast<uint8_t>(ev.update.type);
    header.bid_count = ev.update.bids.size();
    header.ask_count = ev.update.asks.size();
    header.exchange_ts_ns = ev.update.exchangeTsNs;

    std::vector<BookLevel> bids, asks;
    for (const auto& b : ev.update.bids) {
      bids.push_back({b.price.raw(), b.quantity.raw()});
    }
    for (const auto& a : ev.update.asks) {
      asks.push_back({a.price.raw(), a.quantity.raw()});
    }

    _writer.writeBook(header, bids, asks);
  }

  void flush() { _writer.flush(); }
  void close() { _writer.close(); }
  WriterStats stats() const { return _writer.stats(); }

private:
  static WriterConfig createConfig(const std::filesystem::path& dir) {
    WriterConfig cfg;
    cfg.output_dir = dir;
    cfg.max_segment_bytes = 256 << 20;
    cfg.create_index = true;
    return cfg;
  }

  BinaryLogWriter _writer;
};

// Wire it up
auto recorder = std::make_unique<CustomBusRecorder>("/data/live");
tradeBus->subscribe(recorder.get());
bookBus->subscribe(recorder.get());
```

## 4. Writer Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `output_dir` | Required | Directory for segment files |
| `output_filename` | Auto | Override auto-generated filename |
| `max_segment_bytes` | 256 MB | Rotate to new file at this size |
| `buffer_size` | 64 KB | Write buffer size |
| `sync_on_rotate` | true | fsync before starting new segment |
| `create_index` | true | Write index for fast seeking |
| `index_interval` | 1000 | Events between index entries |
| `compression` | None | `None` or `LZ4` |

## 5. File Format

FLOX creates `.floxlog` segment files with embedded index:

```
/data/market_data/
├── metadata.json                    # Recording metadata (exchange, symbols, etc.)
├── 20250101_120000_000.floxlog      # First segment (index embedded at end)
├── 20250101_121500_001.floxlog      # Second segment (after rotation)
└── index.floxidx                    # Optional: global index across all segments
```

Segment structure:
```
┌──────────────────┐
│  SegmentHeader   │  Magic, version, compression, timestamps, index_offset
├──────────────────┤
│  Event Frames    │  Trade and book update records
│  ...             │
├──────────────────┤
│  SegmentIndex    │  Embedded: timestamp → offset mapping (if create_index=true)
└──────────────────┘
```

The global index (`index.floxidx`) can be built separately using `GlobalIndexBuilder` to enable fast lookup across multiple segment files.

## 6. Recording Metadata

Each recording includes a `metadata.json` file with source information:

```json
{
  "exchange": "binance",
  "exchange_type": "cex",
  "instrument_type": "perpetual",

  "symbols": [
    {
      "symbol_id": 1,
      "name": "BTCUSDT",
      "base_asset": "BTC",
      "quote_asset": "USDT",
      "price_precision": 2,
      "qty_precision": 3
    }
  ],

  "has_trades": true,
  "has_book_snapshots": true,
  "has_book_deltas": true,
  "book_depth": 20,

  "recording_start": "2025-01-15T10:30:00.000Z",
  "recording_end": "2025-01-15T18:45:00.000Z",

  "price_scale": 100000000,
  "qty_scale": 100000000
}
```

### Using BinaryLogRecorderHook

`flox::replay::BinaryLogRecorderHook` plugs into a Runner / LiveEngine
via `flox_runner_set_market_data_recorder` and writes both trades and
books on the engine thread:

```cpp
#include "flox/replay/binary_log_recorder_hook.h"

flox::replay::BinaryLogRecorderHookConfig cfg;
cfg.output_dir = "/data/btcusdt";
cfg.max_segment_bytes = 256ull << 20;
cfg.exchange_id = 0;
cfg.compression = flox::replay::CompressionType::None;

flox::replay::BinaryLogRecorderHook hook(std::move(cfg));

flox::replay::SymbolInfo sym;
sym.symbol_id = 1;
sym.name = "BTCUSDT";
sym.base_asset = "BTC";
sym.quote_asset = "USDT";
sym.price_precision = 2;
sym.qty_precision = 3;
hook.addSymbol(sym);

// Attach via flox_runner_set_market_data_recorder (see the C-API
// recorder-hook section); start/stop fire automatically with the
// runner. metadata.json is written on stop().
```

### Manual Metadata with BinaryLogWriter

```cpp
#include "flox/replay/recording_metadata.h"

RecordingMetadata meta;
meta.exchange = "bybit";
meta.instrument_type = "spot";
meta.has_trades = true;

WriterConfig config;
config.output_dir = "/data/spot";
config.metadata = meta;

BinaryLogWriter writer(config);
writer.addSymbol({.symbol_id = 1, .name = "ETHUSDT"});
// ... write events ...
writer.close();  // Saves metadata.json
```

## 7. Monitoring Recording

```cpp
// Periodically check stats
WriterStats stats = writer.stats();
std::cout << "Events: " << stats.events_written
          << ", Trades: " << stats.trades_written
          << ", Books: " << stats.book_updates_written
          << ", Segments: " << stats.segments_created
          << ", Bytes: " << stats.bytes_written << std::endl;
```

## 8. Compression

Enable LZ4 for 3-5x size reduction:

```bash
cmake .. -DFLOX_ENABLE_LZ4=ON
```

```cpp
WriterConfig config;
config.compression = CompressionType::LZ4;
// Everything else works the same
```

## Next Steps

- [Backtesting](backtesting.md) — Replay recorded data through your strategy
- [Binary Format](../reference/api/replay/binary_format.md) — Recording format specification
