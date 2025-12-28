# Recording Data

Capture live market data to disk for later replay and backtesting.

## Prerequisites

- Completed [First Strategy](first-strategy.md)
- Optional: LZ4 library for compression (`-DFLOX_ENABLE_LZ4=ON`)

## 1. BinaryLogWriter Overview

FLOX records market data in a custom binary format optimized for:

- Sequential writes (no random access during recording)
- Fast sequential reads during replay
- Optional LZ4 compression
- Automatic file rotation

```cpp
#include "flox/replay/writers/binary_log_writer.h"

using namespace flox::replay;
```

## 2. Basic Recording

```cpp
// Configure the writer
WriterConfig config;
config.output_dir = "/data/market_data";      // Output directory
config.max_segment_bytes = 256 * 1024 * 1024; // 256 MB per segment
config.create_index = true;                    // Enable fast seeking
config.compression = CompressionType::None;    // Or CompressionType::LZ4

BinaryLogWriter writer(config);

// Write a trade
TradeRecord trade;
trade.symbol_id = 42;
trade.price = 10050;        // Fixed-point (multiply by 100)
trade.quantity = 100;       // Fixed-point
trade.side = 1;             // 1=buy, 0=sell
trade.exchange_ts_ns = 1234567890123456789;  // Nanoseconds since epoch

writer.writeTrade(trade);

// Write a book update
BookRecordHeader header;
header.symbol_id = 42;
header.update_type = static_cast<uint8_t>(BookUpdateType::SNAPSHOT);
header.bid_count = 5;
header.ask_count = 5;
header.exchange_ts_ns = 1234567890123456789;

std::vector<BookLevel> bids = { {10049, 100}, {10048, 200}, ... };
std::vector<BookLevel> asks = { {10051, 150}, {10052, 250}, ... };

writer.writeBook(header, bids, asks);

// Ensure data is persisted
writer.flush();
writer.close();
```

## 3. Recording from Live Connectors

Subscribe the writer to your market data buses:

```cpp
class MarketDataRecorder : public IMarketDataSubscriber
{
public:
  MarketDataRecorder(const std::filesystem::path& output_dir)
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
auto recorder = std::make_unique<MarketDataRecorder>("/data/live");
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

### Using MarketDataRecorder

The high-level `MarketDataRecorder` automatically creates metadata:

```cpp
#include "flox/replay/market_data_recorder.h"

MarketDataRecorderConfig config;
config.output_dir = "/data/btcusdt";
config.exchange_name = "binance";
config.exchange_type = "cex";
config.instrument_type = "perpetual";
config.book_depth = 20;

MarketDataRecorder recorder(config);

// Add symbol mappings
recorder.addSymbol(1, "BTCUSDT", "BTC", "USDT", 2, 3);

recorder.start();
// ... subscribe to market data buses ...
recorder.stop();  // Writes metadata.json automatically
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
- [Replay System Reference](../reference/replay.md) — Full API documentation
