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

## Key Methods

| Method                  | Description                                                     |
| ----------------------- | --------------------------------------------------------------- |
| `subscribe(listener)`   | Registers a consumer with optional `required` flag for gating.  |
| `publish(event)`        | Publishes event to ring buffer, returns sequence number.        |
| `start()` / `stop()`    | Starts or stops all consumer threads.                           |
| `waitConsumed(seq)`     | Blocks until all consumers have processed up to `seq`.          |
| `flush()`               | Waits until all published events are consumed.                  |
| `consumerCount()`       | Returns number of registered consumers.                         |
| `enableDrainOnStop()`   | Ensures remaining events are dispatched before shutdown.        |

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

## Design Highlights

* **Disruptor Pattern**: Single producer, multiple consumers with sequence-based coordination.
* **Ring Buffer**: Fixed-size power-of-2 capacity with wrap-around.
* **Busy-Spin Waiting**: Uses `BusyBackoff` for low-latency polling.
* **Gating Sequence**: Publishers wait for slowest required consumer before overwriting.
* **Per-Consumer Threads**: Each consumer runs in dedicated `std::jthread`.
* **Zero Allocations**: Events stored directly in pre-allocated ring buffer slots.
* **Tick Sequencing**: `tickSequence` field is automatically set if present on event.
* **In-Place Construction**: Events constructed via placement new, destructed on reclaim.

## Internal Types

| Name           | Description                                                |
| -------------- | ---------------------------------------------------------- |
| `ConsumerSlot` | Per-consumer state: listener, sequence, thread, required.  |
| `Listener`     | Inferred from `Event::Listener` via `ListenerType` trait.  |

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
bus.subscribe(&bookHandler);

#if FLOX_CPU_AFFINITY_ENABLED
bus.setupOptimalConfiguration(BookBus::ComponentType::MARKET_DATA);
#endif

bus.start();
bus.publish(std::move(bookUpdateHandle));
bus.flush();
bus.stop();
```

## Notes

* Capacity must be a power of 2 for efficient masking.
* Non-required consumers (second arg to `subscribe`) don't block publisher.
* `enableDrainOnStop()` should be called before `start()` if drain behavior is needed.
* CPU affinity features require `FLOX_CPU_AFFINITY_ENABLED` compile flag.
