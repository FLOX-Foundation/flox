# Replay System

The replay system enables recording live market data and replaying it for backtesting, analysis, and strategy development.

## Overview

```
┌────────────┐     ┌─────────────────┐     ┌───────────────────┐
│  Live Feed │────▶│ BinaryLogWriter │────▶│ .floxseg files    │
└────────────┘     └─────────────────┘     └─────────┬─────────┘
                                                     │
                   ┌─────────────────┐               │
                   │ ReplayConnector │◀──────────────┘
                   └────────┬────────┘
                            │
                   ┌────────▼────────┐
                   │    Strategy     │
                   └─────────────────┘
```

## Recording Market Data

Use `BinaryLogWriter` to record live data to segments:

```cpp
#include "flox/replay/writers/binary_log_writer.h"

replay::WriterConfig config{
    .data_dir = "/data/market",
    .segment_duration_ns = 3600LL * 1e9,  // 1 hour segments
    .create_index = true,
    .compression = CompressionType::LZ4
};

replay::BinaryLogWriter writer(config);

// In your connector callback:
connector->setCallbacks(
    [&writer](const BookUpdateEvent& ev) {
        writer.writeBookUpdate(ev);
    },
    [&writer](const TradeEvent& ev) {
        writer.writeTrade(ev);
    }
);
```

## Replaying Data

### Basic Replay

```cpp
#include "flox/replay/replay_connector.h"

ReplayConnectorConfig config{
    .data_dir = "/data/market",
    .speed = ReplaySpeed::max()  // As fast as possible
};

auto replay = std::make_shared<ReplayConnector>(config);

replay->setCallbacks(
    [](const BookUpdateEvent& ev) { /* handle book */ },
    [](const TradeEvent& ev) { /* handle trade */ }
);

replay->start();

while (!replay->isFinished()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

### With Engine

```cpp
auto registry = std::make_unique<SymbolRegistry>();
auto mdb = std::make_unique<MarketDataBus>();

ReplayConnectorConfig replay_config{
    .data_dir = "/data/market",
    .speed = ReplaySpeed::max()
};

auto replay = std::make_shared<ReplayConnector>(replay_config);
replay->setCallbacks(
    [&mdb](const BookUpdateEvent& ev) { mdb->publish(ev); },
    [&mdb](const TradeEvent& ev) { mdb->publish(ev); }
);

auto strategy = std::make_shared<MyStrategy>();
mdb->subscribe(strategy);

std::vector<std::shared_ptr<IExchangeConnector>> connectors{replay};
std::vector<std::unique_ptr<ISubsystem>> subsystems;
subsystems.push_back(std::move(mdb));

Engine engine(config, std::move(subsystems), std::move(connectors));
engine.start();
```

## Playback Speed

```cpp
// Maximum speed (no delays) - use for backtesting
ReplaySpeed::max()

// Real-time playback
ReplaySpeed::realtime()

// 10x faster than realtime
ReplaySpeed::fast(10.0)

// Change speed during replay
replay->setSpeed(ReplaySpeed::fast(5.0));
```

## Time Filtering

```cpp
ReplayConnectorConfig config{
    .data_dir = "/data/market",
    .from_ns = start_timestamp,  // Start from this time
    .to_ns = end_timestamp       // End at this time
};
```

## Symbol Filtering

```cpp
ReplayConnectorConfig config{
    .data_dir = "/data/market",
    .symbols = {1, 2, 3}  // Only replay these symbol IDs
};
```

## Reading Without Connector

For analysis without the connector interface:

### Sequential Read

```cpp
#include "flox/replay/readers/binary_log_reader.h"

replay::ReaderConfig config{
    .data_dir = "/data/market",
    .from_ns = start_time,
    .to_ns = end_time
};

replay::BinaryLogReader reader(config);

reader.forEach([](const replay::ReplayEvent& event) {
    if (event.type == replay::EventType::Trade) {
        // Process trade
    } else {
        // Process book update
    }
    return true;  // Continue
});
```

### Memory-Mapped Read

```cpp
#include "flox/replay/readers/mmap_reader.h"

replay::MmapReader::Config config{
    .data_dir = "/data/market",
    .preload_index = true
};

replay::MmapReader reader(config);

// Zero-copy access to trade records
reader.forEachRawTrade([](const replay::TradeRecord* trade) {
    // Direct pointer to mapped memory
    return true;
});
```

### Parallel Read

```cpp
#include "flox/replay/readers/parallel_reader.h"

replay::ParallelReaderConfig config{
    .data_dir = "/data/market",
    .num_threads = 4
};

replay::ParallelReader reader(config);

// Events arrive in timestamp order across threads
reader.forEach([](const replay::ReplayEvent& event) {
    return true;
});

auto stats = reader.stats();
std::cout << stats.eventsPerSecond() << " events/sec\n";
```

## Dataset Inspection

```cpp
auto summary = replay::BinaryLogReader::inspect("/data/market");

std::cout << "Segments: " << summary.segment_count << "\n";
std::cout << "Events: " << summary.total_events << "\n";
std::cout << "Duration: " << summary.durationHours() << " hours\n";
std::cout << "Size: " << summary.total_bytes / (1024*1024) << " MB\n";
```

## Segment Operations

### Merge Segments

```cpp
#include "flox/replay/ops/segment_ops.h"

replay::MergeConfig config{
    .output_dir = "/data/merged",
    .output_name = "combined",
    .compression = CompressionType::LZ4
};

auto result = replay::SegmentOps::mergeDirectory("/data/segments", config);
```

### Split by Time

```cpp
replay::SplitConfig config{
    .output_dir = "/data/hourly",
    .mode = replay::SplitMode::ByTime,
    .time_interval_ns = 3600LL * 1e9  // 1 hour
};

auto result = replay::SegmentOps::split("/data/day.floxseg", config);
```

### Export to CSV

```cpp
replay::ExportConfig config{
    .output_path = "/data/trades.csv",
    .format = replay::ExportFormat::CSV,
    .trades_only = true
};

auto result = replay::SegmentOps::exportData("/data/market.floxseg", config);
```

## Data Validation

```cpp
#include "flox/replay/ops/validator.h"

replay::DatasetValidator validator;
auto result = validator.validate("/data/market");

if (!result.valid) {
    std::cerr << "Corrupted: " << result.corrupted_segments << " segments\n";
    std::cerr << "Errors: " << result.total_errors << "\n";
}
```

## Best Practices

### Recording

1. Use LZ4 compression for 2-3x size reduction with minimal CPU overhead
2. Create indexes for fast timestamp-based seeking
3. Use reasonable segment sizes (1 hour is typical)
4. Store symbol registry alongside data for ID resolution

### Backtesting

1. Use `ReplaySpeed::max()` for fastest execution
2. Filter by time range to focus on specific periods
3. Filter by symbols to reduce memory usage
4. Validate data before critical backtests

### Performance

| Reader          | Use Case                              |
|-----------------|---------------------------------------|
| BinaryLogReader | General purpose, compressed data      |
| MmapReader      | Zero-copy, uncompressed data          |
| ParallelReader  | High throughput, multiple threads     |
| ReplayConnector | Engine integration, speed control     |

## File Format

Segments use the `.floxseg` extension with a compact binary format:

- 64-byte aligned structures for direct memory mapping
- CRC32 checksums on all frames
- Optional LZ4 compression
- Optional seek indexes for fast timestamp lookups

See [Binary Format](../components/replay/binary_format.md) for specification.
