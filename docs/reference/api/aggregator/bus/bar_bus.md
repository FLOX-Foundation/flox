# BarBus

`BarBus` is the event bus for distributing `BarEvent` to subscribers.

```cpp
using BarBus = EventBus<BarEvent>;

std::unique_ptr<BarBus> createOptimalBarBus(bool enablePerformanceOptimizations = false);
bool configureBarBusForPerformance(BarBus& bus, bool enablePerformanceOptimizations = false);
```

## Factory Functions

### createOptimalBarBus

Creates a new `BarBus` with optional CPU affinity optimization.

```cpp
auto bus = createOptimalBarBus(true);  // Enable performance optimizations
```

### configureBarBusForPerformance

Configures an existing `BarBus` for optimal performance.

```cpp
BarBus bus;
configureBarBusForPerformance(bus, true);
```

## Example Usage

```cpp
BarBus bus;
bus.enableDrainOnStop();  // Flush pending bars on stop

// Subscribe a strategy
bus.subscribe(&myStrategy);

bus.start();

// Bars are published by BarAggregator
// myStrategy.onBar() receives BarEvent

bus.stop();
```

## See Also

* [BarEvent](../events/bar_event.md) — Event structure
* [Bar](../bar.md) — Bar data structure
* [EventBus](../../util/eventing/event_bus.md) — Generic event bus
