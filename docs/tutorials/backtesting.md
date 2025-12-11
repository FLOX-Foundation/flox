# Backtesting

Replay recorded market data through your strategy.

## Prerequisites

- Completed [Recording Data](recording-data.md)
- Recorded `.floxlog` files

## 1. ReplayConnector Overview

`ReplayConnector` reads recorded data and emits events to your strategy as if they were live.

```cpp
#include "flox/replay/replay_connector.h"

using namespace flox;
```

## 2. Basic Replay

```cpp
// Configure replay
ReplayConnectorConfig config;
config.data_dir = "/data/market_data";        // Directory with .floxlog files
config.speed = ReplaySpeed::max();            // As fast as possible

// Create replay connector
auto replay = std::make_shared<ReplayConnector>(config);

// Set up callbacks (same as live connectors)
replay->setTradeCallback([](const TradeEvent& ev) {
  // Process trade
});

replay->setBookCallback([](const BookUpdateEvent& ev) {
  // Process book update
});

// Run
replay->start();
// ... wait for completion ...
replay->stop();
```

## 3. Replay with Strategy

Wire the replay connector to your strategy:

```cpp
#include "flox/book/bus/trade_bus.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/engine/engine.h"
#include "flox/replay/replay_connector.h"

// Create buses
auto tradeBus = std::make_unique<TradeBus>();
auto bookBus = std::make_unique<BookUpdateBus>();

// Create your strategy
auto strategy = std::make_unique<MyStrategy>(/*symbolId=*/0);
tradeBus->subscribe(strategy.get());
bookBus->subscribe(strategy.get());

// Configure replay
ReplayConnectorConfig replayConfig;
replayConfig.data_dir = "/data/market_data";
replayConfig.speed = ReplaySpeed::max();

auto replay = std::make_shared<ReplayConnector>(replayConfig);

// Connect replay to buses
replay->setTradeCallback([&tradeBus](const TradeEvent& ev) {
  tradeBus->publish(ev);
});

replay->setBookCallback([&bookBus](const BookUpdateEvent& ev) {
  // For pooled events, acquire from pool first
  auto handle = bookPool.acquire();
  if (handle) {
    **handle = ev;
    bookBus->publish(std::move(handle));
  }
});

// Build engine
std::vector<std::unique_ptr<ISubsystem>> subsystems;
subsystems.push_back(std::move(tradeBus));
subsystems.push_back(std::move(bookBus));
subsystems.push_back(std::move(strategy));

std::vector<std::shared_ptr<IExchangeConnector>> connectors;
connectors.push_back(replay);

EngineConfig config{};
Engine engine(config, std::move(subsystems), std::move(connectors));

// Run backtest
engine.start();
while (!replay->isFinished()) {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
engine.stop();
```

## 4. Controlling Replay Speed

```cpp
// Real-time (1x speed)
config.speed = ReplaySpeed::realtime();

// 10x faster than real-time
config.speed = ReplaySpeed::multiplied(10.0);

// As fast as possible (no throttling)
config.speed = ReplaySpeed::max();

// Change speed during replay
replay->setSpeed(ReplaySpeed::multiplied(5.0));
```

## 5. Time Range Filtering

Replay only a portion of your data:

```cpp
ReplayConnectorConfig config;
config.data_dir = "/data/market_data";

// Filter by time range (nanoseconds since Unix epoch)
config.from_ns = 1704067200000000000LL;  // 2024-01-01 00:00:00 UTC
config.to_ns = 1704153600000000000LL;    // 2024-01-02 00:00:00 UTC
```

## 6. Symbol Filtering

Replay only specific symbols:

```cpp
config.symbols = {0, 1, 5};  // Only symbol IDs 0, 1, and 5
```

## 7. Inspecting Data Before Replay

Check what's in your dataset:

```cpp
#include "flox/replay/readers/binary_log_reader.h"

using namespace flox::replay;

// Quick summary
DatasetSummary summary = BinaryLogReader::inspect("/data/market_data");
std::cout << "Events: " << summary.total_events << std::endl;
std::cout << "Duration: " << summary.durationMinutes() << " minutes" << std::endl;
std::cout << "Segments: " << summary.segment_count << std::endl;
std::cout << "Indexed: " << summary.fullyIndexed() << std::endl;

// With symbol list
DatasetSummary detailed = BinaryLogReader::inspectWithSymbols("/data/market_data");
std::cout << "Symbols: ";
for (uint32_t sym : detailed.symbols) {
  std::cout << sym << " ";
}
std::cout << std::endl;
```

## 8. Seeking Within Data

Jump to a specific timestamp (requires indexed data):

```cpp
// Seek to specific time
bool success = replay->seekTo(1704100000000000000LL);
if (!success) {
  std::cerr << "Seek failed (data not indexed?)" << std::endl;
}

// Get current position
int64_t pos = replay->currentPosition();
```

## 9. Reading Data Directly

For custom analysis without the connector:

```cpp
ReaderConfig config;
config.data_dir = "/data/market_data";
config.from_ns = std::nullopt;  // No time filter
config.to_ns = std::nullopt;
config.verify_crc = true;

BinaryLogReader reader(config);

// Iterate all events
reader.forEach([](const ReplayEvent& ev) {
  if (ev.type == EventType::Trade) {
    std::cout << "Trade: symbol=" << ev.trade.symbol_id
              << " price=" << ev.trade.price << std::endl;
  } else if (ev.type == EventType::Book) {
    std::cout << "Book: symbol=" << ev.book_header.symbol_id
              << " bids=" << ev.bids.size()
              << " asks=" << ev.asks.size() << std::endl;
  }
  return true;  // Continue iteration
});

// Stats
ReaderStats stats = reader.stats();
std::cout << "Read " << stats.events_read << " events" << std::endl;
```

## 10. Backtest Performance Tips

1. **Use `ReplaySpeed::max()`** for fastest backtesting
2. **Disable logging** during backtest runs
3. **Filter symbols** to reduce processing
4. **Use indexed data** for time range queries
5. **Build with `-DCMAKE_BUILD_TYPE=Release -O3`**

## Next Steps

- [Optimize Performance](../how-to/optimize-performance.md) — Tune for minimum latency
- [Replay System Reference](../reference/replay.md) — Full API documentation
