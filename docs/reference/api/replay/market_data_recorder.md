# MarketDataRecorder

`MarketDataRecorder` is a high-level component that subscribes to market data buses and records events to binary log files. It implements `IMarketDataSubscriber` for seamless integration with the engine.

```cpp
struct MarketDataRecorderConfig {
  std::filesystem::path output_dir;
  uint64_t max_segment_bytes{256ull << 20};  // 256 MB
  uint8_t exchange_id{0};
};

class MarketDataRecorder : public IMarketDataRecorder {
public:
  explicit MarketDataRecorder(MarketDataRecorderConfig config);
  ~MarketDataRecorder() override;

  // ISubsystem
  void start() override;
  void stop() override;

  // IMarketDataSubscriber
  SubscriberId id() const override;
  void onBookUpdate(const BookUpdateEvent& event) override;
  void onTrade(const TradeEvent& event) override;
  void onCandle(const CandleEvent& event) override;

  // IMarketDataRecorder
  void setOutputDir(const std::filesystem::path& dir) override;
  void flush() override;
  RecorderStats stats() const override;
  bool isRecording() const override;
};
```

## Purpose

* Provide a ready-to-use market data recording solution.
* Subscribe to `TradeBus`, `BookUpdateBus`, and `CandleBus` for automatic recording.
* Abstract away low-level binary format details.

## Configuration

| Field | Default | Description |
|-------|---------|-------------|
| `output_dir` | - | Directory for recorded segments. |
| `max_segment_bytes` | 256 MB | Maximum segment size before rotation. |
| `exchange_id` | 0 | Exchange identifier in segment headers. |

## Methods

| Method | Description |
|--------|-------------|
| `start()` | Begin recording (creates initial segment). |
| `stop()` | Stop recording and close current segment. |
| `onBookUpdate(event)` | Record a book update event. |
| `onTrade(event)` | Record a trade event. |
| `onCandle(event)` | Record a candle event (currently no-op). |
| `setOutputDir(dir)` | Change output directory (takes effect on next rotation). |
| `flush()` | Flush buffers to disk. |
| `stats()` | Returns recording statistics. |
| `isRecording()` | Returns `true` if actively recording. |

## Usage

```cpp
// Configure recorder
MarketDataRecorderConfig config{
  .output_dir = "/data/market/btcusdt",
  .max_segment_bytes = 512ull << 20,
  .exchange_id = 1
};

// Create recorder
auto recorder = std::make_shared<MarketDataRecorder>(config);

// Subscribe to buses
tradeBus.subscribe(recorder.get());
bookBus.subscribe(recorder.get());

// Start recording
recorder->start();

// ... market data flows through buses ...

// Stop and flush
recorder->stop();
```

## Integration with Engine

```cpp
// In your builder
auto recorder = std::make_shared<MarketDataRecorder>(recorderConfig);

// Add as subsystem for lifecycle management
subsystems.push_back(recorder);

// Subscribe to market data buses
tradeBus->subscribe(recorder.get());
bookUpdateBus->subscribe(recorder.get());

// Engine will call start()/stop() automatically
```

## Notes

* Implements `ISubsystem` for automatic lifecycle management.
* Thread-safe for concurrent event delivery.
* Internally uses `BinaryLogWriter` for actual file operations.
* Call `flush()` periodically for durability guarantees.
* Segments are automatically rotated based on size.

## See Also

* [BinaryLogWriter](binary_log_writer.md) — Low-level writer
* [BinaryLogReader](binary_log_reader.md) — Reading recorded data
* [Recording Data Tutorial](../../../tutorials/recording-data.md) — Step-by-step guide
* [IMarketDataSubscriber](../engine/abstract_market_data_subscriber.md) — Subscriber interface
