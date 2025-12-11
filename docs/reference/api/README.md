# API Reference

Complete technical documentation for all FLOX components.

## Component Categories

| Category | Description |
|----------|-------------|
| [Engine](engine/engine.md) | Core orchestration and lifecycle management |
| [Book](book/nlevel_order_book.md) | Order book structures and market data |
| [Execution](execution/order.md) | Order management and execution |
| [Replay](replay/binary_log_reader.md) | Data recording and playback |
| [Util](util/eventing/event_bus.md) | Utilities, memory pools, event buses |
| [Connector](connector/exchange_connector.md) | Exchange connectivity |
| [Strategy](strategy/abstract_strategy.md) | Strategy interfaces |
| [Risk](risk/abstract_risk_manager.md) | Risk management |

## Quick Links

### Core

- [Engine](engine/engine.md) — System orchestration
- [EngineConfig](engine/engine_config.md) — Configuration structure
- [SymbolRegistry](engine/symbol_registry.md) — Symbol management

### Market Data

- [Trade](book/trade.md) — Trade structure
- [BookUpdate](book/book_update.md) — Order book updates
- [NLevelOrderBook](book/nlevel_order_book.md) — Order book implementation
- [TradeBus](book/bus/trade_bus.md) — Trade event bus
- [BookUpdateBus](book/bus/book_update_bus.md) — Book update event bus

### Execution

- [Order](execution/order.md) — Order structure
- [AbstractExecutor](execution/abstract_executor.md) — Executor interface
- [OrderExecutionBus](execution/bus/order_execution_bus.md) — Execution event bus

### Utilities

- [EventBus](util/eventing/event_bus.md) — Disruptor-style event bus
- [Pool](util/memory/pool.md) — Object pool
- [Decimal](util/base/decimal.md) — Fixed-point decimal
- [SPSCQueue](util/concurrency/spsc_queue.md) — Lock-free queue

### Replay

- [BinaryLogReader](replay/binary_log_reader.md) — Log reader
- [ReplayConnector](replay/replay_connector.md) — Replay connector
- [Binary Format](replay/binary_format.md) — File format specification

## Header Organization

```
include/flox/
├── book/           # Order book, trades, events
├── connector/      # Exchange connectivity
├── engine/         # Core engine components
├── execution/      # Order execution
├── replay/         # Recording and playback
├── strategy/       # Strategy interfaces
└── util/           # Utilities
```

## See Also

- [Architecture](../../explanation/architecture.md) — How components fit together
- [Integration Flow](../../explanation/integration-flow.md) — Wiring components
