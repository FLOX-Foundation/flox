# Optimize Performance

Tune FLOX for minimum latency.

## Build Optimization

### Release Build

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-O3 -march=native -flto"
```

Default release flags in FLOX:
```cmake
$<$<CONFIG:Release>:-O3 -march=native -flto -funroll-loops>
```

### Link-Time Optimization

Already enabled by default with `-flto`. Ensure your compiler supports it:
```bash
g++ --version  # GCC 13+ recommended
```

## Event Bus Tuning

### Capacity

Default: 4096 events. For high-frequency feeds, increase:

```bash
cmake .. -DFLOX_DEFAULT_EVENTBUS_CAPACITY=16384
```

Or per-bus:
```cpp
using HighCapacityBus = EventBus<TradeEvent, 16384>;
```

Capacity must be power of 2.

### Consumer Limit

Default: 128 consumers. Adjust if needed:

```bash
cmake .. -DFLOX_DEFAULT_EVENTBUS_MAX_CONSUMERS=256
```

## Memory Optimization

### Pre-allocate Pools

Size pools to handle peak load without exhaustion:

```cpp
// For 3 consumers at 10ms processing, 1000 events/sec:
// In-flight ≈ 3 × 10ms × 1000/sec = 30 events
// Add headroom: 64-128

pool::Pool<BookUpdateEvent, 128> bookPool;
```

Monitor pool usage:
```cpp
size_t inUse = bookPool.inUse();
if (inUse > threshold) {
  FLOX_LOG("Warning: pool usage high: " << inUse);
}
```

### Avoid Allocations in Hot Path

**Don't:**
```cpp
void onTrade(const TradeEvent& ev) {
  auto data = std::make_unique<Data>();  // BAD: allocation
  std::string s = std::to_string(ev.trade.price.toDouble());  // BAD
}
```

**Do:**
```cpp
class MyStrategy : public IStrategy {
  Data _data;  // Pre-allocated member
  char _buffer[128];  // Pre-allocated buffer

  void onTrade(const TradeEvent& ev) {
    _data.process(ev);  // Use pre-allocated
    snprintf(_buffer, sizeof(_buffer), "%.2f", ev.trade.price.toDouble());
  }
};
```

## CPU Optimization

### CPU Affinity

See [Configure CPU Affinity](cpu-affinity.md) for details.

Quick setup:
```cpp
#if FLOX_CPU_AFFINITY_ENABLED
tradeBus.setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA, true);
#endif
```

### Disable Frequency Scaling

```bash
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### Isolate CPUs

Kernel parameters:
```
isolcpus=2,3,4,5 nohz_full=2,3,4,5 rcu_nocbs=2,3,4,5
```

## Strategy Optimization

### Filter Early

```cpp
void onTrade(const TradeEvent& ev) {
  // Filter first line — reject early
  if (ev.trade.symbol != _symbol) return;
  if (ev.trade.price < _minPrice) return;

  // Expensive processing only for relevant events
  processSignal(ev);
}
```

### Avoid Branches in Hot Path

```cpp
// BAD: Branch in tight loop
for (const auto& level : book.bids) {
  if (level.price > threshold) {
    total += level.quantity;
  }
}

// BETTER: Branchless or predictable branch
// (compiler may optimize, but be aware)
```

### Cache Friendly Access

```cpp
// BAD: Random access
for (int i : randomIndices) {
  process(data[i]);
}

// GOOD: Sequential access
for (const auto& item : data) {
  process(item);
}
```

## Logging Optimization

### Disable in Hot Path

```cpp
#if NO_COUT
  FLOX_LOG_OFF();
#endif

// Run strategy

#if NO_COUT
  FLOX_LOG_ON();
#endif
```

### Use Conditional Logging

```cpp
#ifdef DEBUG
  FLOX_LOG("Trade: " << ev.trade.price.toDouble());
#endif
```

## Profiling

### Enable Tracy

```bash
cmake .. -DFLOX_ENABLE_TRACY=ON
```

Use profiling macros:
```cpp
void onTrade(const TradeEvent& ev) {
  FLOX_PROFILE_SCOPE("Strategy::onTrade");

  // Your code...
}
```

### Measure Latency

```cpp
class LatencyTracker {
  std::vector<int64_t> _samples;

public:
  void record(int64_t latency_ns) {
    _samples.push_back(latency_ns);
  }

  void report() {
    std::sort(_samples.begin(), _samples.end());
    size_t n = _samples.size();

    std::cout << "p50: " << _samples[n * 0.50] << " ns\n";
    std::cout << "p99: " << _samples[n * 0.99] << " ns\n";
    std::cout << "max: " << _samples.back() << " ns\n";
  }
};
```

## Compression Trade-offs

For replay:
- **No compression:** Fastest read, largest files
- **LZ4:** ~3-5x compression, small CPU overhead

For recording, LZ4 is usually worth it:
```cpp
WriterConfig config;
config.compression = CompressionType::LZ4;  // Good default
```

## Network Optimization

### Socket Tuning

```bash
# Increase receive buffer
sudo sysctl -w net.core.rmem_max=16777216
sudo sysctl -w net.core.rmem_default=16777216
```

### Busy-poll

For lowest latency with kernel 4.11+:
```bash
sudo sysctl -w net.core.busy_poll=50
sudo sysctl -w net.core.busy_read=50
```

## Checklist

- [ ] Release build with `-O3 -march=native -flto`
- [ ] EventBus capacity sized for peak load
- [ ] Object pools pre-allocated
- [ ] No allocations in callbacks
- [ ] CPU affinity configured (if dedicated hardware)
- [ ] CPU frequency scaling disabled
- [ ] Logging disabled during benchmarks
- [ ] Profiling enabled during tuning
- [ ] Socket buffers increased
- [ ] Kernel parameters tuned (isolated CPUs, etc.)

## Benchmarking

Run included benchmarks:
```bash
cmake .. -DFLOX_ENABLE_BENCHMARKS=ON
make -j
./benchmarks/binary_log_benchmark
./benchmarks/nlevel_order_book_benchmark
```

Run your own latency measurements to establish baseline for your hardware.

## See Also

- [Configure CPU Affinity](cpu-affinity.md) — Thread pinning
- [The Disruptor Pattern](../explanation/disruptor.md) — Understanding latency
- [Memory Model](../explanation/memory-model.md) — Zero-allocation design
