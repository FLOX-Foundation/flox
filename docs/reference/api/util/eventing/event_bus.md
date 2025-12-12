# EventBus

`EventBus` is a high-performance, lock-free Disruptor-based messaging system for publishing typed events to multiple subscribers. It uses a ring buffer architecture for zero-allocation event delivery with configurable capacity and consumer count.

```cpp
template <typename Event,
          size_t CapacityPow2 = config::DEFAULT_EVENTBUS_CAPACITY,
          size_t MaxConsumers = config::DEFAULT_EVENTBUS_MAX_CONSUMERS>
class EventBus : public ISubsystem;
```

## Purpose

* Deliver high-frequency events (market data, orders, etc.) to multiple subscribers with minimal latency and zero allocations using the Disruptor pattern.
* Provide backpressure handling via timeout-based publishing.

## Template Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `Event` | - | The event type to publish (must define `Event::Listener`) |
| `CapacityPow2` | 4096 | Ring buffer capacity (must be power of 2) |
| `MaxConsumers` | 128 | Maximum number of subscribers |

## Key Methods

| Method | Description |
|--------|-------------|
| `subscribe(listener, required)` | Registers a subscriber. Returns `bool` (false if bus running or at capacity). |
| `publish(event)` | Publishes event to ring buffer. Returns sequence number (-1 if stopped). |
| `tryPublish(event, timeout)` | Publishes with timeout. Returns `{PublishResult, seq}`. |
| `start()` | Starts all consumer threads. |
| `stop()` | Stops all consumer threads and cleans up. |
| `waitConsumed(seq)` | Blocks until all required consumers have processed up to `seq`. |
| `flush()` | Waits for all published events to be consumed. |
| `enableDrainOnStop()` | Ensures remaining events are dispatched before shutdown. |
| `consumerCount()` | Returns number of registered consumers. |

## PublishResult

```cpp
enum class PublishResult
{
  SUCCESS,   // Event published successfully
  TIMEOUT,   // Buffer full, timeout expired
  STOPPED    // Bus not running
};
```

## Backpressure Handling

When the ring buffer is full, `publish()` blocks. Use `tryPublish()` for non-blocking behavior:

```cpp
auto [result, seq] = bus.tryPublish(event, std::chrono::microseconds{1000});
if (result == Bus::PublishResult::TIMEOUT) {
  // Handle backpressure
}
```

## CPU Affinity Support

When `FLOX_CPU_AFFINITY_ENABLED` is defined, the bus supports advanced thread placement:

```cpp
enum class ComponentType {
  MARKET_DATA,  // Priority 90
  EXECUTION,    // Priority 85
  STRATEGY,     // Priority 80
  RISK,         // Priority 75
  GENERAL       // Priority 70
};

struct AffinityConfig {
  ComponentType componentType = ComponentType::GENERAL;
  bool enableRealTimePriority = true;
  int realTimePriority = 80;
  bool enableNumaAwareness = true;
  bool preferIsolatedCores = true;
};
```

| Method | Description |
|--------|-------------|
| `setAffinityConfig(cfg)` | Configures CPU affinity for consumer threads. |
| `setCoreAssignment(assignment)` | Manually sets core assignment. |
| `setupOptimalConfiguration(type, perf)` | Auto-configures based on component type. |
| `verifyIsolatedCoreConfiguration()` | Verifies isolated cores are properly configured. |

Consumer threads are distributed across available cores using round-robin assignment.

## Internal Types

| Name | Description |
|------|-------------|
| `ConsumerSlot` | Per-consumer state: listener, sequence, thread, required, coreIndex. |
| `Listener` | Inferred from `Event::Listener` (or `pool::Handle<T>::Listener`). |
| `Storage` | Aligned byte array for events in ring buffer. |
| `PublishResult` | Enum for publish outcome. |

## Design Highlights

* **Disruptor pattern**: Single-producer multi-consumer ring buffer with sequence-based coordination.
* **Zero allocations**: Events stored directly in pre-allocated ring buffer slots.
* **Busy-wait with backoff**: `BusyBackoff` for low-latency spinning with exponential backoff.
* **Gating sequences**: Required consumers gate the producer; optional consumers don't block.
* **Automatic reclaim**: Consumed slots are destructed and reclaimed automatically.
* **Cache-line alignment**: All atomics are 64-byte aligned to prevent false sharing.
* **Tick sequence injection**: If event has `tickSequence` field, it's auto-set on publish.
* **Thread-safe subscribe**: Returns false if called after `start()`.
* **Overflow protection**: Sequence counter overflow is detected and handled.

## Example Usage

```cpp
using TradeBus = EventBus<TradeEvent>;
TradeBus bus;

// Subscribe - check return value
if (!bus.subscribe(&tradeHandler)) {
  // Handle error
}
bus.subscribe(&analyticsHandler, false);  // optional, non-blocking

#if FLOX_CPU_AFFINITY_ENABLED
bus.setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA, true);
#endif

bus.start();

// Standard publish
TradeEvent event;
event.trade.price = Price::fromDouble(50000.0);
auto seq = bus.publish(std::move(event));

// Publish with timeout
auto [result, seq2] = bus.tryPublish(event, std::chrono::microseconds{500});
if (result == TradeBus::PublishResult::TIMEOUT) {
  LOG_WARN("Backpressure");
}

bus.waitConsumed(seq);
bus.stop();
```

## Memory Layout

All hot fields are 64-byte aligned to prevent false sharing:

| Field | Alignment | Description |
|-------|-----------|-------------|
| `_running` | cache line | Bus running state |
| `_next` | cache line | Next sequence to publish |
| `_cachedMin` | cache line | Cached minimum gating sequence |
| `_storage[]` | 64-byte | Event ring buffer |
| `_published[]` | cache line | Published sequence per slot |
| `_constructed[]` | cache line | Construction flags |
| `_reclaimSeq` | cache line | Last reclaimed sequence |
| `_consumers[]` | cache line | Consumer slots |
| `_gating[]` | cache line | Gating sequences |

## Required vs Optional Consumers

| Aspect | Required (`true`, default) | Optional (`false`) |
|--------|----------------------------|-------------------|
| **Gating** | Blocks `waitConsumed()` and `flush()` | Does not block |
| **Backpressure** | Can cause publisher to wait | Never causes backpressure |
| **Event delivery** | Guaranteed all events | Guaranteed all events |
| **Reclaim** | Events reclaimed after processing | Events reclaimed after **all** consumers process |

**Key guarantee**: All consumers (required and optional) receive every event, even during wrap-around. Events are only destroyed after all consumers have processed them.

## Notes

* Ring buffer capacity must be a power of 2 for efficient masking.
* Producer blocks if ring buffer is full (use `tryPublish()` for timeout).
* `subscribe()` must be called before `start()`.
* Optional consumers don't block `waitConsumed()` or `flush()`, but still receive all events.
* Events are destructed only after **all** consumers (required and optional) have processed them.
* `publish()` returns -1 if the bus is not running.

## Benchmarking

Run `event_bus_benchmark` to measure performance on your hardware:

```bash
cmake -DFLOX_ENABLE_BENCHMARKS=ON ..
make event_bus_benchmark
./benchmarks/event_bus_benchmark
```

Example results on Intel i5-1135G7 @ 2.40GHz (4 cores / 8 threads):

| Benchmark | Time | Throughput |
|-----------|------|------------|
| PublishLatency | 50 ns | 20 M/s |
| SingleConsumerThroughput | 61 µs/1000 | 16 M/s |
| MultiConsumer (4) | 195 µs/1000 | 5 M/s |
| TryPublishLatency | 110 ns | 9 M/s |
| EndToEndLatency | 200 ns | 5 M/s |

## See Also

* [Disruptor Pattern](../../../../explanation/disruptor.md)
* [Memory Model](../../../../explanation/memory-model.md)
* [Pool](../../memory/pool.md)
