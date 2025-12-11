# EventBus

`EventBus` is a high-performance Disruptor-style ring buffer for broadcasting typed events to multiple consumers. It uses lock-free sequencing with busy-spin waiting for minimal latency.

```cpp
template <typename Event,
          size_t CapacityPow2 = config::DEFAULT_EVENTBUS_CAPACITY,
          size_t MaxConsumers = config::DEFAULT_EVENTBUS_MAX_CONSUMERS>
class EventBus : public ISubsystem;
```

## Purpose

* Deliver high-frequency events (market data, orders, etc.) to multiple subscribers with minimal latency and zero allocations on the hot path.
* Support CPU affinity and real-time thread priority for latency-critical components.
* Provide backpressure handling via timeout-based publishing.

## Key Methods

| Method                  | Description                                                     |
| ----------------------- | --------------------------------------------------------------- |
| `subscribe(listener)`   | Registers a consumer. Returns `bool` (false if bus running or at capacity). |
| `publish(event)`        | Publishes event to ring buffer, returns sequence number (-1 if stopped). |
| `tryPublish(event, timeout)` | Publishes with timeout. Returns `{PublishResult, seq}`. |
| `start()` / `stop()`    | Starts or stops all consumer threads.                           |
| `waitConsumed(seq)`     | Blocks until all consumers have processed up to `seq`.          |
| `flush()`               | Waits until all published events are consumed.                  |
| `consumerCount()`       | Returns number of registered consumers.                         |
| `enableDrainOnStop()`   | Ensures remaining events are dispatched before shutdown.        |

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

When the ring buffer is full (consumers too slow), `publish()` blocks until space is available. Use `tryPublish()` with a timeout to handle backpressure:

```cpp
auto [result, seq] = bus.tryPublish(event, std::chrono::microseconds{1000});
if (result == Bus::PublishResult::TIMEOUT) {
  // Handle backpressure: drop event, log warning, etc.
}
```

## CPU Affinity (when `FLOX_CPU_AFFINITY_ENABLED`)

```cpp
enum class ComponentType
{
  MARKET_DATA,
  EXECUTION,
  STRATEGY,
  RISK,
  GENERAL
};

struct AffinityConfig
{
  ComponentType componentType = ComponentType::GENERAL;
  bool enableRealTimePriority = true;
  int realTimePriority = config::DEFAULT_REALTIME_PRIORITY;
  bool enableNumaAwareness = true;
  bool preferIsolatedCores = true;
};
```

| Method                        | Description                                           |
| ----------------------------- | ----------------------------------------------------- |
| `setAffinityConfig(cfg)`      | Configure CPU affinity and RT priority.               |
| `setCoreAssignment(assign)`   | Manually set core assignment.                         |
| `setupOptimalConfiguration()` | Auto-configure for component type.                    |
| `verifyIsolatedCoreConfig()`  | Verify isolated core setup.                           |

Consumer threads are distributed across available cores using round-robin assignment.

## Design Highlights

* **Disruptor Pattern**: Single producer, multiple consumers with sequence-based coordination.
* **Ring Buffer**: Fixed-size power-of-2 capacity with wrap-around.
* **Busy-Spin Waiting**: Uses `BusyBackoff` for low-latency polling.
* **Gating Sequence**: Publishers wait for slowest required consumer before overwriting.
* **Per-Consumer Threads**: Each consumer runs in dedicated `std::jthread`.
* **Zero Allocations**: Events stored directly in pre-allocated ring buffer slots.
* **Tick Sequencing**: `tickSequence` field is automatically set if present on event.
* **In-Place Construction**: Events constructed via placement new, destructed on reclaim.
* **Thread-Safe Subscribe**: `subscribe()` returns false if called after `start()`.
* **Overflow Protection**: Sequence counter overflow is detected and handled.

## Internal Types

| Name           | Description                                                |
| -------------- | ---------------------------------------------------------- |
| `ConsumerSlot` | Per-consumer state: listener, sequence, thread, required, coreIndex. |
| `Listener`     | Inferred from `Event::Listener` via `ListenerType` trait.  |
| `PublishResult`| Enum for publish outcome (SUCCESS, TIMEOUT, STOPPED).      |

## Template Parameters

| Parameter     | Default                           | Description                    |
| ------------- | --------------------------------- | ------------------------------ |
| `Event`       | -                                 | Event type to broadcast.       |
| `CapacityPow2`| `config::DEFAULT_EVENTBUS_CAPACITY` (4096) | Ring buffer size (power of 2). |
| `MaxConsumers`| `config::DEFAULT_EVENTBUS_MAX_CONSUMERS` (128) | Maximum consumer count.     |

## Example Usage

```cpp
using BookBus = EventBus<pool::Handle<BookUpdateEvent>>;

BookBus bus;

// subscribe() returns bool - check for success
if (!bus.subscribe(&bookHandler)) {
  // Handle error: null listener, bus running, or at capacity
}

#if FLOX_CPU_AFFINITY_ENABLED
bus.setupOptimalConfiguration(BookBus::ComponentType::MARKET_DATA);
#endif

bus.start();

// Standard publish (blocks on backpressure)
auto seq = bus.publish(std::move(bookUpdateHandle));

// Publish with timeout (non-blocking backpressure handling)
auto [result, seq2] = bus.tryPublish(event, std::chrono::microseconds{500});
if (result == BookBus::PublishResult::TIMEOUT) {
  LOG_WARN("Backpressure detected, event dropped");
}

bus.flush();
bus.stop();
```

## Notes

* Capacity must be a power of 2 for efficient masking.
* Non-required consumers (second arg to `subscribe`) don't block publisher.
* `subscribe()` must be called before `start()` - returns false otherwise.
* `enableDrainOnStop()` should be called before `start()` if drain behavior is needed.
* CPU affinity features require `FLOX_CPU_AFFINITY_ENABLED` compile flag.
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
