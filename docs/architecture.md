# Architecture

Flox is a modular framework for building low-latency execution systems. Its design emphasizes **separation of concerns**, **predictable performance**, and **composability**.

## Layers of the Architecture

### 1. Abstract Layer

Defines **pure interfaces** with no internal state. These are the contracts your system is built upon:

* `IStrategy`: strategy logic
* `IOrderExecutor`: order submission
* `IOrderExecutionListener`: execution events
* `IRiskManager`, `IOrderValidator`, `IPositionManager`: trade controls and state
* `IOrderBook`, `ExchangeConnector`: market structure
* `ISubsystem`: unified lifecycle interface
* `IMarketDataSubscriber`: receives events via data bus

#### Why it matters

* Enables simulation, replay, and mocking
* Decouples logic from implementation
* Ensures correctness can be validated independently of performance

### 2. Implementation Layer

* `Engine`: orchestrates startup and shutdown
* `NLevelOrderBook`: in-memory order book with tick-aligned price levels
* `CandleAggregator`: aggregates trades into fixed-interval OHLCV candles
* `SymbolRegistry`: maps `(exchange:symbol)` pairs to compact `SymbolId`
* `EventBus`: event fan-out with push/pull delivery modes and sync/async policy
* `BookUpdateEvent`, `TradeEvent`: pooled, reusable market data structures

#### Features

* **Speed**: tight memory layout, preallocated event structures
* **Control**: no heap allocation in event flow, deterministic dispatch
* **Modularity**: all components are independently replaceable and testable

## Strategy Execution: PUSH and PULL Modes

Strategies implement `IMarketDataSubscriber` and can operate in two modes:

### PUSH Mode (default)

The bus actively delivers events to the strategy:

```cpp
class MyPushStrategy : public IStrategy {
public:
  void onBookUpdate(const BookUpdateEvent& ev) override { /* handle event */ }
};
```

```cpp
marketDataBus->subscribe(strategy);
```

### PULL Mode

The strategy explicitly drains its queue:

```cpp
class MyPullStrategy : public IMarketDataSubscriber {
public:
  SubscriberMode mode() const override { return SubscriberMode::PULL; }

  void readLoop(SPSCQueue<EventHandle<BookUpdateEvent>>& queue) {
    EventHandle<BookUpdateEvent> ev;
    while (queue.pop(ev)) {
      EventDispatcher<EventHandle<BookUpdateEvent>>::dispatch(ev, *this);
    }
  }
};
```

## Market Data Fan-Out: MarketDataBus

The `MarketDataBus` delivers `EventHandle<T>` to each subscriber using dedicated `SPSCQueue`s.

### Publishing:

```cpp
bus->publish(std::move(bookUpdate));
```

### Subscribing:

```cpp
bus->subscribe(myStrategy);
```

### Behavior:

* Each subscriber has an isolated queue
* Events are delivered via `EventDispatcher`
* In `SyncPolicy`, all subscribers are synchronized via `TickBarrier` and `TickGuard`

## Lifecycle and Subsystems

All major components implement `ISubsystem`, exposing `start()` and `stop()` methods.

Benefits:

* Deterministic lifecycle control
* Support for warm-up, teardown, benchmarking
* Simplified simulation and test orchestration

## Memory and Performance

Flox is designed for allocation-free execution on the hot path:

* `BookUpdateEvent`, `TradeEvent` come from `Pool<T>`
* `Handle<T>` ensures safe ref-counted reuse
* `SPSCQueue` provides lock-free delivery
* `std::pmr::vector` used in `BookUpdate` avoids heap churn

## Symbol-Centric Design

All routing and lookup is based on `SymbolId` (`uint32_t`):

* Fast lookup, avoids string comparison
* Enables per-symbol state machines, queues, books
* Supports dense fan-out architectures

## Intended Use

Flox is not a full trading engine — it’s a **toolkit** for building:

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
marketDataBus->subscribe(strategy);

marketDataBus->publish(std::move(bookUpdate));
```

In pull-mode:

```cpp
auto* queue = marketDataBus->getQueue(strategy->id());
EventHandle<BookUpdateEvent> ev;
while (queue->pop(ev)) {
  EventDispatcher<EventHandle<BookUpdateEvent>>::dispatch(ev, *strategy);
}
```

## Summary

Flox is:

* **Modular** — use only what you need
* **Deterministic** — fully controlled event timing
* **Safe** — no hidden allocations, pooled memory
* **Flexible** — works in backtests, simulation, and live systems

You define the logic — Flox moves the data.
