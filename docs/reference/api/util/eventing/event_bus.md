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

## Template Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `Event` | - | The event type to publish (must define `Event::Listener`) |
| `CapacityPow2` | 4096 | Ring buffer capacity (must be power of 2) |
| `MaxConsumers` | 128 | Maximum number of subscribers |

## Key Methods

| Method | Description |
|--------|-------------|
| `subscribe(listener, required)` | Registers a subscriber. `required=true` means it participates in gating. |
| `publish(event)` | Publishes an event to the ring buffer. Returns sequence number. |
| `start()` | Starts all consumer threads. |
| `stop()` | Stops all consumer threads and cleans up. |
| `waitConsumed(seq)` | Blocks until all required consumers have processed up to `seq`. |
| `flush()` | Waits for all published events to be consumed. |
| `enableDrainOnStop()` | Ensures remaining events are dispatched before shutdown. |
| `consumerCount()` | Returns number of registered consumers. |

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

## Internal Types

| Name | Description |
|------|-------------|
| `ConsumerSlot` | Per-consumer state: listener pointer, sequence, thread, required flag. |
| `Listener` | Inferred from `Event::Listener` (or `pool::Handle<T>::Listener`). |
| `Storage` | Aligned storage for events in ring buffer. |

## Design Highlights

* **Disruptor pattern**: Single-producer multi-consumer ring buffer with sequence-based coordination.
* **Zero allocations**: Events stored directly in pre-allocated ring buffer slots.
* **Busy-wait with backoff**: `BusyBackoff` for low-latency spinning with exponential backoff.
* **Gating sequences**: Required consumers gate the producer; optional consumers don't block.
* **Automatic reclaim**: Consumed slots are destructed and reclaimed automatically.
* **Cache-line alignment**: All atomics are 64-byte aligned to prevent false sharing.
* **Tick sequence injection**: If event has `tickSequence` field, it's auto-set on publish.

## Example Usage

```cpp
// Create a trade bus with default capacity
using TradeBus = EventBus<TradeEvent>;
TradeBus bus;

// Subscribe handlers
bus.subscribe(&tradeHandler);
bus.subscribe(&analyticsHandler, false);  // optional, non-blocking

// Configure CPU affinity (optional)
#if FLOX_CPU_AFFINITY_ENABLED
bus.setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA, true);
#endif

// Start consumer threads
bus.start();

// Publish events
TradeEvent event;
event.trade.price = Price::fromDouble(50000.0);
auto seq = bus.publish(std::move(event));

// Wait for consumption if needed
bus.waitConsumed(seq);

// Cleanup
bus.stop();
```

## Memory Layout

```
┌─────────────────────────────────────────────────┐
│ _running (atomic<bool>)         [cache line 0] │
├─────────────────────────────────────────────────┤
│ _next (atomic<int64_t>)         [cache line 1] │
├─────────────────────────────────────────────────┤
│ _cachedMin (atomic<int64_t>)    [cache line 2] │
├─────────────────────────────────────────────────┤
│ _storage[CapacityPow2]          [aligned]      │
├─────────────────────────────────────────────────┤
│ _published[CapacityPow2]        [cache line]   │
├─────────────────────────────────────────────────┤
│ _constructed[CapacityPow2]      [cache line]   │
├─────────────────────────────────────────────────┤
│ _consumers[MaxConsumers]        [cache line]   │
├─────────────────────────────────────────────────┤
│ _gating[MaxConsumers]           [cache line]   │
└─────────────────────────────────────────────────┘
```

## Notes

* Ring buffer capacity must be a power of 2 for efficient masking.
* Producer blocks if ring buffer is full (all consumers must catch up).
* `required=false` consumers are skipped in gating calculation (useful for monitoring).
* Events are destructed immediately after all consumers have processed them.

## See Also

* [Disruptor Pattern](../../../../explanation/disruptor.md) — How the ring buffer works
* [Memory Model](../../../../explanation/memory-model.md) — Zero-allocation event delivery
* [Pool](../../memory/pool.md) — Object pooling for large events
