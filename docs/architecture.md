# Architecture

FLOX is a modular framework for building low-latency execution systems. Its design emphasizes **separation of concerns**, **predictable performance**, and **composability**.

## Layers of the Architecture

### 1. Abstract Layer

Defines **pure interfaces** with no internal state. These are the contracts your system is built upon:

* `IStrategy`: strategy logic
* `IOrderExecutor`: order submission
* `IOrderExecutionListener`: execution events
* `IRiskManager`, `IOrderValidator`, `IPositionManager`: trade controls and state
* `IOrderBook`, `IExchangeConnector`: market structure
* `ISubsystem`: unified lifecycle interface
* `IMarketDataSubscriber`: receives events via data bus

#### Why it matters

* Enables simulation, replay, and mocking
* Decouples logic from implementation
* Ensures correctness can be validated independently of performance

### 2. Implementation Layer

* `Engine`: orchestrates startup and shutdown
* `NLevelOrderBook`: in-memory order book with tick-aligned price levels
* `BarAggregator`: aggregates trades into fixed-interval OHLCV bars
* `SymbolRegistry`: maps `(exchange:symbol)` pairs to compact `SymbolId`
* `EventBus`: Disruptor-style ring buffer for high-throughput event delivery
* `BookUpdateEvent`, `TradeEvent`: pooled, reusable market data structures

#### Features

* **Speed**: tight memory layout, preallocated event structures
* **Control**: no heap allocation in event flow, deterministic dispatch
* **Modularity**: all components are independently replaceable and testable

## Event Delivery

Strategies implement `IMarketDataSubscriber` and receive events via `EventBus`:

```cpp
class MyStrategy : public IMarketDataSubscriber
{
public:
  SubscriberId id() const override { return reinterpret_cast<SubscriberId>(this); }

  void onBookUpdate(const BookUpdateEvent& ev) override { /* handle event */ }
  void onTrade(const TradeEvent& ev) override { /* handle event */ }
};
```

```cpp
marketDataBus->subscribe(&strategy);
marketDataBus->start();
```

## Market Data Fan-Out: EventBus

The `EventBus` uses a Disruptor-pattern ring buffer for high-throughput event delivery:

### Publishing:

```cpp
bus->publish(std::move(bookUpdate));
```

### Subscribing:

```cpp
bus->subscribe(&myStrategy);
```

### Behavior:

* Single producer, multiple consumers
* Lock-free sequencing with busy-spin waiting
* Gating prevents overwriting unconsumed events
* Per-consumer threads with optional CPU affinity
* Events dispatched via `EventDispatcher`

### CPU Affinity (optional):

```cpp
#if FLOX_CPU_AFFINITY_ENABLED
bus->setupOptimalConfiguration(BookUpdateBus::ComponentType::MARKET_DATA);
#endif
```

## Lifecycle and Subsystems

All major components implement `ISubsystem`, exposing `start()` and `stop()` methods.

Benefits:

* Deterministic lifecycle control
* Support for warm-up, teardown, benchmarking
* Simplified simulation and test orchestration

## Memory and Performance

FLOX is designed for allocation-free execution on the hot path:

* `BookUpdateEvent`, `TradeEvent` come from `Pool<T>`
* `Handle<T>` ensures safe ref-counted reuse
* `EventBus` ring buffer avoids dynamic allocation
* `std::pmr::vector` used in `BookUpdate` avoids heap churn

## Symbol-Centric Design

All routing and lookup is based on `SymbolId` (`uint32_t`):

* Fast lookup, avoids string comparison
* Enables per-symbol state machines, queues, books
* Supports dense fan-out architectures

## Intended Use

FLOX is not a full trading engine — it's a **toolkit** for building:

* Real-time trading systems
* Simulators and replay backtesters
* Signal fan-out and market data routers
* Custom HFT infrastructure

Designed for teams that require:

* Predictable low-latency performance
* Explicit memory and thread control
* Modular, testable architecture

## Example Integration

```cpp
auto strategy = std::make_shared<MyStrategy>();

BookUpdateBus bus;
bus.subscribe(strategy.get());
bus.start();

// Publishing events
bus.publish(std::move(bookUpdate));

// Clean shutdown
bus.flush();
bus.stop();
```

## Summary

FLOX is:

* **Modular** — use only what you need
* **Deterministic** — fully controlled event timing
* **Safe** — no hidden allocations, pooled memory
* **Flexible** — works in backtests, simulation, and live systems

You define the logic — FLOX moves the data.
