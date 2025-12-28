# Reference

Technical specifications and API documentation.

## Overview

| Section | Contents |
|---------|----------|
| [Engine & Lifecycle](engine.md) | Core engine, subsystems, configuration |
| [Market Data](market-data.md) | Events, buses, order books |
| [Execution](execution.md) | Orders, executors, listeners |
| [Replay System](replay.md) | Recording, playback, segment operations |
| [Utilities](utilities.md) | Pools, queues, decimal types |

## Quick Reference

### Common Types

```cpp
using SymbolId = uint32_t;
using OrderId = uint64_t;
using UnixNanos = int64_t;
using SubscriberId = uintptr_t;
```

### Key Interfaces

| Interface | Purpose |
|-----------|---------|
| `ISubsystem` | Lifecycle management (`start()`, `stop()`) |
| `IStrategy` | Trading strategy |
| `IMarketDataSubscriber` | Receives market data events |
| `IExchangeConnector` | Exchange connection |
| `IOrderExecutor` | Order submission |
| `IRiskManager` | Risk checks |

### Event Buses

| Bus | Event Type | Header |
|-----|------------|--------|
| `TradeBus` | `TradeEvent` | `flox/book/bus/trade_bus.h` |
| `BookUpdateBus` | `pool::Handle<BookUpdateEvent>` | `flox/book/bus/book_update_bus.h` |
| `BarBus` | `BarEvent` | `flox/aggregator/bus/bar_bus.h` |
| `OrderExecutionBus` | `OrderEvent` | `flox/execution/bus/order_execution_bus.h` |
