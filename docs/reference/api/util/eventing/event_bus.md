# EventBus

`EventBus` is a Disruptor-style ring buffer for broadcasting typed events to multiple consumers. Uses lock-free sequencing with busy-spin waiting.

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
| `subscribe(listener, required, wait)` | Registers a consumer. `required=true` (default) gates publishing. `wait` is `ConsumerWaitMode::ACTIVE` (default, spins) or `PARKED` (blocks, publisher wakes it). Returns `bool`. |
| `publish(event)`        | Publishes event to ring buffer, returns sequence number (-1 if stopped). |
| `publishBatch(evs, count)` | Publishes a contiguous batch, returns the last sequence. -1 if stopped, if `count == 0`, or if `count > CapacityPow2 / 2`. |
| `tryPublish(event, timeout)` | Publishes with timeout. Returns `{PublishResult, seq}`. |
| `start()` / `stop()`    | Starts or stops all consumer threads. `stop()` returns only once no publisher is inside the ring. |
| `waitConsumed(seq)`     | Blocks until all **required** consumers have processed up to `seq`. |
| `flush()`               | Waits until all published events are consumed by **required** consumers. |
| `consumerCount()`       | Returns number of registered consumers.                         |
| `enableDrainOnStop()`   | Ensures remaining events are dispatched before shutdown.        |
| `setOwnConsumerThreads(bool)` | Before `start()`. `false` spawns no consumer threads: you step them. |
| `pollConsumer(i)`       | One step over consumer `i`; `false` means nothing was there. One stepper per consumer. |
| `drainConsumer(i)`      | Hands consumer `i` everything left in the ring, uncapped.        |
| `consumerFailed(i)`     | The listener threw; the slot is out of service and steps on it do nothing. |
| `consumerHealth(i)`     | The last observed health of one consumer. Readable at any time, from any thread. |
| `healthSnapshot()`      | Counts of stalled and dead consumers from the last sweep, without re-sweeping. |
| `consumerHealthReport(i)` | State, `lastSeen` and `lastChange` for one consumer, from a single sweep. |
| `monitorPeriod(threshold)` | Static. The period the built-in monitor thread sleeps between sweeps. |

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

`TIMEOUT` means the event never entered the ring: no slot was written and no
sequence number was spent, so the next accepted publish takes the sequence this
one would have had. `stats().dropped` counts the refusals. See
[The Disruptor Pattern](../../../../explanation/disruptor.md) for why the two
publish paths claim their sequence at different points.

## Batch Publishing

```cpp
int64_t lastSeq = bus.publishBatch(evs, count);
```

`count` must be between 1 and `CapacityPow2 / 2`; anything else is refused with
-1, in every build, and nothing is published. The upper bound is the ring's, not
a style rule: a range wider than the ring reserves slots it wraps back onto, so
the wrap gate would wait on sequences inside the same batch that only this
publisher can stamp. The refusal is a return, never a block.

## Stopping

`stop()` takes the bus down in this order: publishing is closed, the publishers
already inside the ring are waited out, consumer threads are joined (draining
first if `enableDrainOnStop()` was set), and only then are the remaining events
destroyed and the gating lines reset.

The contract that follows from it:

* A publish is either refused -- `publish()` returns -1, `publishBatch()`
  returns -1, `tryPublish()` returns `STOPPED` -- or its event is in the ring.
  An accepted event is never thrown away by the shutdown before the consumers
  that were going to get it have had their chance (with `enableDrainOnStop()`,
  that means delivered; without it, the undrained tail is dropped as
  documented below).
* When `stop()` returns, no publisher is writing into a slot any more, so the
  bus can be destroyed or restarted.
* `stop()` waits for a publisher that is inside the ring, including one parked
  in the event's own copy constructor. A publisher blocked at the wrap gate or
  the reclaim fence gives up instead of waiting, so a stalled consumer does not
  keep `stop()` from returning.
* Sequence numbers restart from zero on the next `start()`, so they are unique
  within a run and not across runs.

## Consumer Health

`consumerHealth(i)`, `healthSnapshot()` and `consumerHealthReport(i)` are
level-triggered queries: they read what the last sweep recorded, from any
thread, while the sweep is running and after the bus has stopped.

```cpp
const auto report = bus.consumerHealthReport(0);
// report.state      -- HEALTHY, STALLED or DEAD
// report.lastSeen   -- the sequence this consumer had reached
// report.lastChange -- when that sequence last moved
```

The three fields come from one sweep: `lastChange` moves only together with
`lastSeen`, so the pair says whether the consumer is progressing and, if not,
since when. A state on its own does not.

With `enableMonitorThread`, the sweep runs on a thread of the bus's own.
`monitorPeriod(stallThreshold)` is the period it sleeps between sweeps: half the
threshold, floored at 1 ms. The floor matters because the arithmetic is integer
milliseconds -- without it a threshold under 2 ms halves to zero and the monitor
holds a core. An idle bus costs no measurable CPU at any threshold.

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
| `verifyIsolatedCoreConfiguration()` | Verify isolated core setup.                     |

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

## Statistics

```cpp
struct Stats
{
  uint64_t published{0};
  uint64_t dropped{0};
  uint64_t consumed{0};
};

Stats stats() const;
```

`stats()` snapshots three relaxed-load counters. The counts are not a consistent triple — each is
read independently — so treat them as monotonic indicators, not as an invariant
(`published == consumed + dropped` may not hold at the instant of the call).

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

## Required vs Optional Consumers

Consumers can be registered as **required** (default) or **optional**:

```cpp
bus.subscribe(&criticalHandler, true);   // required (default)
bus.subscribe(&loggingHandler, false);   // optional
```

### Behavior differences

| Aspect | Required Consumer | Optional Consumer |
|--------|-------------------|-------------------|
| **Gating** | Blocks `waitConsumed()` and `flush()` | Does not block these methods |
| **Backpressure** | Can cause publisher to wait | Never causes backpressure |
| **Event delivery** | Always receives all events | Always receives all events |
| **Reclaim** | Events reclaimed after processing | Events reclaimed after **all** consumers process |

### Key guarantee

**All consumers (required and optional) are guaranteed to receive every event**, even during ring buffer wrap-around. The bus ensures events are not destroyed until all consumers have processed them.

### Use cases

* **Required**: Strategy handlers, risk managers, order routers - anything that must process every event
* **Optional**: Logging, metrics, debugging tools - where occasional delays shouldn't block the main flow

## Notes

* Capacity must be a power of 2 for efficient masking.
* Optional consumers don't block `waitConsumed()` or `flush()`, but still receive all events.
* `subscribe()` must be called before `start()` - returns false otherwise.
* `enableDrainOnStop()` should be called before `start()` if drain behavior is needed.
* CPU affinity features require `FLOX_CPU_AFFINITY_ENABLED` compile flag.
* `publish()` returns -1 if the bus is not running.
* `publishBatch()` returns -1 for an empty batch and for one larger than half the ring.

## Benchmarking

Run `event_bus_benchmark` to measure performance on your hardware:

```bash
cmake -DFLOX_BUILD_BENCHMARKS=ON ..
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
