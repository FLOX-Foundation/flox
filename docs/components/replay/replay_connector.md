# ReplayConnector

`ReplayConnector` implements `IReplaySource` to replay recorded market data through the standard connector interface. It supports variable playback speed, seeking, and deterministic backtesting.

```cpp
struct ReplayConnectorConfig
{
  std::filesystem::path data_dir;
  ReplaySpeed speed{ReplaySpeed::max()};
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;
};

class ReplayConnector : public IReplaySource
{
public:
  explicit ReplayConnector(ReplayConnectorConfig config);

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "replay"; }

  std::optional<TimeRange> dataRange() const override;
  void setSpeed(ReplaySpeed speed) override;
  bool seekTo(int64_t timestamp_ns) override;
  bool isFinished() const override;
  int64_t currentPosition() const override;
};
```

## Purpose

* Provide a drop-in replacement for live connectors during backtesting.
* Emit `BookUpdateEvent` and `TradeEvent` through the standard connector interface.
* Support deterministic, reproducible replays at maximum speed.

## ReplaySpeed

```cpp
struct ReplaySpeed
{
  double multiplier{0.0};

  static ReplaySpeed realtime() { return {1.0}; }
  static ReplaySpeed fast(double x) { return {x}; }
  static ReplaySpeed max() { return {0.0}; }

  bool isMax() const { return multiplier <= 0.0; }
  bool isRealtime() const { return multiplier == 1.0; }
};
```

| Mode         | Multiplier | Behavior                              |
|--------------|------------|---------------------------------------|
| `max()`      | 0.0        | No delays, process as fast as possible |
| `realtime()` | 1.0        | Wall-clock timing matches event timestamps |
| `fast(x)`    | x > 0      | x times faster than realtime          |

## Configuration

| Field    | Type                | Description                             |
|----------|---------------------|-----------------------------------------|
| data_dir | `filesystem::path`  | Directory containing recorded segments  |
| speed    | `ReplaySpeed`       | Playback speed control                  |
| from_ns  | `optional<int64_t>` | Start timestamp filter                  |
| to_ns    | `optional<int64_t>` | End timestamp filter                    |
| symbols  | `set<uint32_t>`     | Symbol IDs to replay                    |

## IReplaySource Interface

| Method            | Description                                    |
|-------------------|------------------------------------------------|
| `start()`         | Begin replay in background thread              |
| `stop()`          | Stop replay and join thread                    |
| `dataRange()`     | Returns time range of available data           |
| `setSpeed()`      | Change playback speed during replay            |
| `seekTo()`        | Jump to a specific timestamp                   |
| `isFinished()`    | Returns true when all events have been emitted |
| `currentPosition()` | Returns current replay timestamp             |

## Usage

```cpp
ReplayConnectorConfig config{
    .data_dir = "/data/market",
    .speed = ReplaySpeed::max()  // Fastest for backtesting
};

auto connector = std::make_shared<ReplayConnector>(config);

connector->setCallbacks(
    [](const BookUpdateEvent& ev) { /* handle book */ },
    [](const TradeEvent& ev) { /* handle trade */ }
);

connector->start();

// Wait for replay to complete
while (!connector->isFinished()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

connector->stop();
```

### With Engine Integration

```cpp
Engine engine(config);

auto replay = std::make_shared<ReplayConnector>(replay_config);
engine.addConnector(replay);

engine.start();
// Events flow through the standard connector pipeline
```

### Speed Control

```cpp
// Start at realtime speed
connector->setSpeed(ReplaySpeed::realtime());

// Speed up 10x
connector->setSpeed(ReplaySpeed::fast(10.0));

// Switch to max speed
connector->setSpeed(ReplaySpeed::max());
```

### Seeking

```cpp
// Jump to specific timestamp
connector->seekTo(target_timestamp_ns);

// Get current position
int64_t pos = connector->currentPosition();
```

## Notes

* At `max()` speed, events are emitted without any wall-clock delays.
* The replay thread runs independently; events are emitted via callbacks.
* Seeking creates a new reader instance positioned at the target timestamp.
* `isFinished()` returns true only after all matching events are emitted.
* Deterministic: same input data always produces same event sequence.
