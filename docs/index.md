# Flox

**Flox** is a modular C++ framework for building ultra-low-latency execution systems.
It provides a strict separation between abstract interfaces and high-performance implementations, designed for:

* Real-time and historical trading infrastructure
* Event-driven architecture with tick-level granularity
* Unified simulation and live execution environments
* Deterministic latency and memory control

## Highlights

* Pluggable exchange connectors via `ExchangeConnector` and `ConnectorFactory`
* Lock-free `EventBus` with support for both sync and async fan-out
* Fast tick-aligned in-memory order books (`NLevelOrderBook`)
* Strategy layer with explicit lifecycle and bus integration
* Unified `IMarketDataSubscriber` interface for book, trade, and candle events
* Modular design: each component is testable, swappable, and lifecycle-aware
* Explicit memory and object reuse via pooled `Handle<T>` and `Pool<T>`
