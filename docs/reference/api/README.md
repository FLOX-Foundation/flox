# API Reference

Complete technical documentation for all FLOX components.

## Component Categories

| Category | Description |
|----------|-------------|
| [Engine](engine/engine.md) | Core orchestration and lifecycle management |
| [Book](book/nlevel_order_book.md) | Order book structures and market data |
| [Execution](execution/order.md) | Order management and execution |
| [Replay](replay/binary_log_reader.md) | Data recording and playback |
| [Backtest](backtest/backtest_runner.md) | Backtesting and optimization |
| [Util](util/eventing/event_bus.md) | Utilities, memory pools, event buses |
| [Connector](connector/exchange_connector.md) | Exchange connectivity |
| [Strategy](strategy/abstract_strategy.md) | Strategy interfaces |
| [Risk](risk/abstract_risk_manager.md) | Risk management |
| [Aggregator](aggregator/bar.md) | Bar aggregation, footprint, volume and market profiles |
| [Position](position/position_tracker.md) | Position tracking and reconciliation |
| [CEX](cex/index.md) | Multi-exchange primitives: composite book, order router, split orders |
| [C API](capi/flox_capi.md) | The C FFI surface every binding calls into |
| [Metrics](metrics/abstract_pnl_tracker.md) | PnL and execution trackers |
| [Log](log/log.md) | Logging |
| [Net](net/abstract_transport.md) | HTTP and WebSocket transports |
| [KillSwitch](killswitch/abstract_killswitch.md) | Kill-switch interface |
| [Validation](validation/abstract_order_validator.md) | Order validation interface |
| [Sink](sink/abstract_storage_sink.md) | Storage sink interface |
| [Common](common.md) | Shared enums, identifiers and fixed-point types |

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

### Backtest

- [BacktestRunner](backtest/backtest_runner.md) — Backtest executor with interactive mode
- [BacktestResult](backtest/backtest_result.md) — Results container and stats
- [BacktestOptimizer](backtest/backtest_optimizer.md) — Grid search optimizer
- [OptimizationStatistics](backtest/optimization_stats.md) — Statistical analysis
- [MmapBarStorage](backtest/mmap_bar_storage.md) — Memory-mapped bar data

## Header Organization

`include/flox/` has 27 subdirectories plus `common.h`. The ones with reference pages here:

```
include/flox/
├── aggregator/     # Bar aggregation, footprint, volume/market profiles
├── backtest/       # Backtesting and optimization
├── book/           # Order book, trades, events
├── capi/           # C FFI surface
├── connector/      # Exchange connectivity
├── engine/         # Core engine components
├── exchange/       # Exchange metadata
├── execution/      # Order execution, order router, split orders
├── killswitch/     # Kill-switch interface
├── log/            # Logging
├── metrics/        # Metrics collection
├── net/            # HTTP / WebSocket transports
├── position/       # Position tracking and reconciliation
├── replay/         # Recording and playback
├── risk/           # Risk management
├── sink/           # Storage sinks
├── strategy/       # Strategy interfaces, signals, symbol context
├── util/           # Utilities
├── validation/     # Order validation
└── common.h        # Shared enums, ids, fixed-point types
```

The remaining directories — `error/`, `feed/`, `indicator/`, `pricing/`, `report/`, `run/`, `stats/`,
`target/`, `testing/` — have no page under `docs/reference/api/` yet; read the headers directly.

Two documentation directories do not map one-to-one onto a header directory: `cex/` collects the
multi-exchange primitives that live under `execution/`, `position/` and `exchange/`, and
`aggregator/custom/` headers are documented under `aggregator/`.

## See Also

- [Architecture](../../explanation/architecture.md) — How components fit together
- [Integration Flow](../../explanation/integration-flow.md) — Wiring components
