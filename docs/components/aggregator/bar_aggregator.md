# Bar Aggregator

The Bar Aggregator system provides flexible bar generation from trade data with support for multiple bar types, timeframes, and custom aggregated structures.

## Overview

The aggregator system is built around a **policy-based design** with zero-cost abstractions:

```
TradeEvent ──► BarAggregator<Policy> ──► BarEvent ──► BarMatrix
                     │                        │
                     ▼                        ▼
              [Time|Tick|Volume|...]    Strategy.onBar()
                                              │
                                              ▼
                                    bars[symbol][timeframe][idx]
```

## Quick Start

```cpp
#include "flox/aggregator/bar_aggregator.h"
#include "flox/aggregator/bus/bar_bus.h"

// Create a 1-minute time bar aggregator
BarBus bus;
TimeBarAggregator aggregator(TimeBarPolicy(std::chrono::seconds(60)), &bus);

// Subscribe to bar events
bus.subscribe(&strategy);

// Start
bus.start();
aggregator.start();

// Feed trades
aggregator.onTrade(tradeEvent);

// Stop (flushes remaining bars)
aggregator.stop();
bus.stop();
```

## Bar Types

### Time Bars

Close after a fixed time interval.

```cpp
TimeBarAggregator aggregator(TimeBarPolicy(std::chrono::seconds(60)), &bus);
```

**Use cases**: Traditional OHLCV charts, backtesting, most strategies.

### Tick Bars

Close after a fixed number of trades.

```cpp
TickBarAggregator aggregator(TickBarPolicy(100), &bus);  // 100-tick bars
```

**Use cases**: High-frequency trading, eliminating time-based noise, volume-normalized analysis.

### Volume Bars

Close after a fixed notional volume.

```cpp
VolumeBarAggregator aggregator(VolumeBarPolicy::fromDouble(1000000.0), &bus);  // $1M bars
```

**Use cases**: Volume-weighted analysis, consistent information content per bar.

### Renko Bars

Close when price moves by a fixed amount (brick size).

```cpp
RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(10.0), &bus);  // $10 bricks
```

**Use cases**: Trend following, noise elimination, support/resistance identification.

### Range Bars

Close when high-low range exceeds a threshold.

```cpp
RangeBarAggregator aggregator(RangeBarPolicy::fromDouble(5.0), &bus);  // $5 range
```

**Use cases**: Volatility-based analysis, breakout detection.

## Bar Structure

```cpp
struct Bar {
  Price open, high, low, close;
  Volume volume;          // Notional volume (price * quantity)
  Volume buyVolume;       // Volume from buy trades (for delta calculation)
  Quantity tradeCount;    // Number of trades in bar
  TimePoint startTime;    // Bar open time
  TimePoint endTime;      // Bar close time
  BarCloseReason reason;  // Why bar closed (Threshold, Gap, Forced, Warmup)
};

// Calculate delta (buy pressure - sell pressure)
Volume delta = bar.buyVolume.raw() - (bar.volume.raw() - bar.buyVolume.raw());
```

## Multi-Timeframe Analysis

Use `MultiTimeframeAggregator` to produce multiple timeframes from a single trade stream:

```cpp
MultiTimeframeAggregator<4> aggregator(&bus);
aggregator.addTimeInterval(std::chrono::seconds(60));    // M1
aggregator.addTimeInterval(std::chrono::seconds(300));   // M5
aggregator.addTimeInterval(std::chrono::seconds(3600));  // H1
aggregator.addTickInterval(100);                          // 100-tick bars

bus.subscribe(&strategy);
bus.start();
aggregator.start();

// All trades go to all timeframes
aggregator.onTrade(trade);
```

### Mixed Bar Types

You can mix time, tick, and volume bars in a single aggregator:

```cpp
aggregator.addTimeInterval(std::chrono::seconds(60));
aggregator.addTickInterval(50);
aggregator.addVolumeInterval(100000.0);
```

## Bar History: BarSeries

`BarSeries` is a ring buffer for storing bar history:

```cpp
BarSeries<256> series;  // Last 256 bars

series.push(bar);

// Access (0 = newest, 1 = previous, etc.)
const Bar& latest = series[0];
const Bar& previous = series[1];

// Iteration (newest to oldest)
for (const auto& bar : series) {
  // ...
}
```

## Multi-Symbol Multi-Timeframe: BarMatrix

`BarMatrix` provides O(1) access to bar history across symbols and timeframes:

```cpp
BarMatrix<256, 8, 64> matrix;  // 256 symbols, 8 timeframes, 64 bars depth

std::array<TimeframeId, 3> tfs = {timeframe::M1, timeframe::M5, timeframe::H1};
matrix.configure(tfs);

// Subscribe to receive bars
bus.subscribe(&matrix);

// Access: matrix[symbol][timeframe][index]
const Bar* bar = matrix.bar(symbolId, timeframe::H1, 0);  // Latest H1 bar
const Bar* prev = matrix.bar(symbolId, timeframe::H1, 1); // Previous H1 bar

// Or by timeframe index
const Bar* bar = matrix.bar(symbolId, 0, 0);  // First configured timeframe
```

### Warmup with Historical Data

```cpp
std::vector<Bar> historicalBars = loadFromDatabase();
matrix.warmup(symbolId, timeframe::H1, historicalBars);
```

## TimeframeId

`TimeframeId` encodes bar type and parameter:

```cpp
// Presets
timeframe::M1   // 1 minute
timeframe::M5   // 5 minutes
timeframe::M15  // 15 minutes
timeframe::H1   // 1 hour
timeframe::H4   // 4 hours
timeframe::D1   // 1 day

// Custom
TimeframeId tf = TimeframeId::time(std::chrono::seconds(120));  // 2 minutes
TimeframeId tick = TimeframeId::tick(500);                       // 500 ticks
TimeframeId vol = TimeframeId::volume(1000000);                  // $1M volume
```

## Custom Policies

Implement your own bar policy by satisfying the `BarPolicy` concept:

```cpp
struct MyCustomPolicy {
  static constexpr BarType kBarType = BarType::Custom;

  bool shouldClose(const TradeEvent& trade, const Bar& bar) noexcept {
    // Your closing logic
    return /* condition */;
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept {
    updateOHLCV(trade, bar);  // Use helper for OHLCV update
    // Additional custom updates
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept {
    initializeBar(trade, bar);  // Use helper for initialization
    // Additional initialization
  }
};

// Use with BarAggregator
BarAggregator<MyCustomPolicy> aggregator(MyCustomPolicy{...}, &bus);
```

## BarEvent

Bar events contain full bar data plus metadata:

```cpp
struct BarEvent {
  SymbolId symbol;
  InstrumentType instrument;
  BarType barType;
  uint32_t barTypeParam;  // seconds, tick count, volume threshold
  Bar bar;
};
```

## Strategy Integration

### Using BarStrategy Helper

```cpp
class MyStrategy : public BarStrategy<4> {
public:
  void setBarMatrix(BarMatrix<>* matrix) {
    BarStrategy::setBarMatrix(matrix);
  }

  void onBar(const BarEvent& ev) override {
    // Access bars via helper methods
    auto* h1 = bar(timeframe::H1, 0);
    auto* h1_prev = bar(timeframe::H1, 1);

    // Or use optional-returning methods
    auto closeOpt = close(timeframe::H1, 0);
    if (closeOpt && *closeOpt > *close(timeframe::H1, 1)) {
      // H1 closed higher
    }
  }
};
```

### Manual Integration

```cpp
class MyStrategy : public IMarketDataSubscriber {
  void onBar(const BarEvent& ev) override {
    if (ev.barType == BarType::Time && ev.barTypeParam == 60) {
      // Handle 1-minute bars
    }
  }
};
```

## Performance

| Operation | Complexity |
|-----------|------------|
| Policy shouldClose() | O(1), inlined |
| Symbol lookup | O(1) via SymbolStateMap |
| Bar history access | O(1) ring buffer |
| Timeframe lookup | O(n), n ≤ 8 |
| Bar push | O(1) amortized |

Benchmark results (GCC 13.1, LTO):
- TimeBarAggregator.onTrade: ~50ns/trade
- MultiTimeframeAggregator (4 TF): ~60ns/trade
- BarMatrix random access: ~5ns

## Files

| File | Description |
|------|-------------|
| `aggregator/bar.h` | Bar struct, BarType, BarCloseReason |
| `aggregator/timeframe.h` | TimeframeId, presets |
| `aggregator/aggregation_policy.h` | BarPolicy concept |
| `aggregator/bar_aggregator.h` | BarAggregator<Policy> template |
| `aggregator/multi_timeframe_aggregator.h` | Multi-TF aggregator |
| `aggregator/bar_series.h` | Ring buffer for history |
| `aggregator/bar_matrix.h` | Multi-symbol multi-TF storage |
| `aggregator/events/bar_event.h` | BarEvent struct |
| `aggregator/bus/bar_bus.h` | EventBus<BarEvent> |
| `aggregator/policies/*.h` | Time, Tick, Volume, Renko, Range policies |

## See Also

- [Volume Profile](volume_profile.md) - Volume distribution analysis
- [Footprint Chart](footprint_chart.md) - Order flow analysis
- [Market Profile](market_profile.md) - TPO-based session analysis
