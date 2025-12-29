# ExchangeClockSync

Clock synchronization for exchanges using RTT-based offset estimation with EMA smoothing.

## Header

```cpp
#include "flox/util/sync/exchange_clock_sync.h"
```

## Synopsis

```cpp
template <size_t MaxExchanges = 8>
class ExchangeClockSync
{
public:
  struct ClockEstimate {
    int64_t offsetNs{0};       // exchange - local (nanoseconds)
    int64_t confidenceNs{0};   // ± uncertainty (2 sigma)
    int64_t latencyNs{0};      // one-way estimate
    uint32_t sampleCount{0};
  };

  // Record timing sample with validation
  bool recordSample(ExchangeId exchange,
                    int64_t localSendNs,
                    int64_t exchangeNs,
                    int64_t localRecvNs) noexcept;

  // Get clock estimate
  ClockEstimate estimate(ExchangeId exchange) const noexcept;

  // Time conversion
  int64_t toLocalTimeNs(ExchangeId exchange, int64_t exchangeNs) const noexcept;
  int64_t toExchangeTimeNs(ExchangeId exchange, int64_t localNs) const noexcept;

  // Sync status
  bool hasSync(ExchangeId exchange) const noexcept;
  bool hasReliableSync(ExchangeId exchange) const noexcept;

  // Reset
  void reset(ExchangeId exchange) noexcept;
  void resetAll() noexcept;

  static constexpr uint32_t kMinSamplesForReliable = 10;
};
```

## Offset Calculation

The clock offset is estimated using the classic NTP algorithm:

```
RTT = localRecv - localSend
oneWay = RTT / 2
offset = exchangeTs - (localSend + oneWay)
```

Samples are smoothed using EMA (alpha = 0.1):
```cpp
offset = (offset * 9 + rawOffset) / 10;
latency = (latency * 9 + oneWay) / 10;
```

## Sample Validation

Samples are rejected if:
- RTT ≤ 0 (impossible)
- RTT > 10 seconds (network issue)
- Exchange time > 1 hour behind local (clock drift)

```cpp
bool accepted = sync.recordSample(exchange, localSend, exchangeTs, localRecv);
if (!accepted) {
  // Sample rejected - invalid timing
}
```

## Usage

### Recording Samples

```cpp
ExchangeClockSync<4> sync;

// Record timing from API call
auto localSend = std::chrono::steady_clock::now().time_since_epoch().count();
auto response = exchange.getServerTime();
auto localRecv = std::chrono::steady_clock::now().time_since_epoch().count();

sync.recordSample(exchangeId, localSend, response.serverTime, localRecv);
```

### Checking Sync Status

```cpp
if (!sync.hasSync(exchangeId)) {
  // No samples recorded yet
}

if (sync.hasReliableSync(exchangeId)) {
  // At least 10 samples, estimate is reliable
}
```

### Getting Clock Estimate

```cpp
auto est = sync.estimate(exchangeId);
std::cout << "Offset: " << est.offsetNs / 1e6 << " ms\n";
std::cout << "Latency: " << est.latencyNs / 1e6 << " ms\n";
std::cout << "Confidence: ±" << est.confidenceNs / 1e6 << " ms\n";
std::cout << "Samples: " << est.sampleCount << "\n";
```

### Time Conversion

```cpp
// Convert exchange timestamp to local time
int64_t localTs = sync.toLocalTimeNs(exchangeId, exchangeTs);

// Convert local timestamp to exchange time
int64_t exchangeTs = sync.toExchangeTimeNs(exchangeId, localTs);
```

### Using with OrderRouter

```cpp
OrderRouter<4> router;
ExchangeClockSync<4> clockSync;

// Record samples during normal operation
// ...

// Use for lowest-latency routing
router.setClockSync(&clockSync);
router.setRoutingStrategy(RoutingStrategy::LowestLatency);
```

## Performance

| Operation | Complexity | Latency |
|-----------|------------|---------|
| recordSample() | O(1) | <1ns |
| estimate() | O(1) | <1ns |
| toLocalTimeNs() | O(1) | <1ns |
| hasSync() | O(1) | <1ns |

## Confidence Interval

The confidence interval is calculated as 2σ of the offset variance:

```cpp
// Variance accumulator (EMA)
int64_t diff = rawOffset - offset;
varianceAcc = (varianceAcc * 9 + diff * diff) / 10;

// Confidence = 2 * sqrt(variance)
confidenceNs = 2 * sqrt(varianceAcc);
```

## See Also

- [OrderRouter](order_router.md) - Uses clock sync for LowestLatency routing
