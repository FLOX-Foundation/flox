# Architecture Overview

How FLOX components fit together. The diagrams below describe the runtime engine — the same architecture you get from every binding (Python, Node.js, Codon, C++). Bindings are thin wrappers; the engine layout is identical.

## System Layers

```mermaid
flowchart TB
    subgraph L3["Application Layer"]
        strategy[Your Strategy]
    end

    subgraph L2["Core Layer"]
        eventbus[EventBus]
        execution[Order Execution]
        risk[Risk Management]
    end

    subgraph L1["Infrastructure Layer"]
        connectors[Connectors]
        orderbooks[Order Books]
        registry[Symbol Registry]
        replay[Replay]
    end

    L1 --> L2
    L2 --> L3
```

| Layer | Components | Purpose |
|-------|------------|---------|
| **Infrastructure** | Connectors, Replay, Symbol Registry, Order Books | Low-level I/O and data management |
| **Core** | EventBus, Order Execution, Risk Management | Event routing and order flow |
| **Application** | Your Strategy | Trading logic |

## Data Flow

```mermaid
flowchart TD
    subgraph External
        EX[Exchange API]
    end

    subgraph Connectors
        CONN[IExchangeConnector]
    end

    subgraph EventBuses[Event Buses - Disruptor Ring Buffers]
        TB[TradeBus]
        BB[BookUpdateBus]
        CB[BarBus]
    end

    subgraph Aggregators
        CA[BarAggregator]
    end

    subgraph Strategies
        ST[IStrategy]
    end

    subgraph Execution
        OEB[OrderExecutionBus]
        EXE[IOrderExecutor]
        RM[IRiskManager]
        KS[IKillSwitch]
    end

    EX --> CONN
    CONN --> TB
    CONN --> BB
    TB --> ST
    BB --> ST
    TB --> CA
    CA --> CB
    CB --> ST
    ST -->|Order| RM
    RM -->|Allowed| KS
    KS -->|Not Triggered| EXE
    EXE --> OEB
```

## Core Components

### Engine

The `Engine` class orchestrates the system lifecycle:

```cpp
class Engine : public ISubsystem
{
public:
  Engine(const EngineConfig& config,
         std::vector<std::unique_ptr<ISubsystem>> subsystems,
         std::vector<std::shared_ptr<IExchangeConnector>> connectors);

  void start() override;
  void stop() override;
};
```

- Takes ownership of all subsystems
- Starts subsystems first, then connectors
- Stops connectors first, then subsystems
- No configuration file parsing — you wire components manually

### Event Buses

All buses use the Disruptor pattern (see [The Disruptor Pattern](disruptor.md)):

| Bus | Event Type | Purpose |
|-----|------------|---------|
| `TradeBus` | `TradeEvent` | Individual trades |
| `BookUpdateBus` | `pool::Handle<BookUpdateEvent>` | Order book snapshots/deltas |
| `BarBus` | `BarEvent` | OHLCV bars |
| `OrderExecutionBus` | `OrderEvent` | Order state changes |

Key characteristics:

- Lock-free ring buffer
- Single producer, multiple consumers
- Consumers run in dedicated threads
- Backpressure via sequence gating

### Connectors

`IExchangeConnector` interface:

```cpp
class IExchangeConnector
{
public:
  virtual ~IExchangeConnector() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual std::string exchangeId() const = 0;
};
```

Connectors:

- Parse exchange-specific wire protocols
- Convert to FLOX event types
- Publish to event buses
- Run their own network threads

### Strategies

`IStrategy` combines `ISubsystem` + `IMarketDataSubscriber`:

```cpp
class IStrategy : public ISubsystem, public IMarketDataSubscriber
{
public:
  virtual ~IStrategy() = default;
};
```

From `IMarketDataSubscriber`:

- `onTrade(const TradeEvent&)`
- `onBookUpdate(const BookUpdateEvent&)`
- `onBar(const BarEvent&)`

From `ISubsystem`:

- `start()`
- `stop()`

## Subsystem Interface

Everything that participates in engine lifecycle implements:

```cpp
class ISubsystem
{
public:
  virtual ~ISubsystem() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
};
```

Subsystems include:

- Event buses
- Strategies
- Aggregators (e.g., BarAggregator)
- Execution trackers
- Custom components

## Symbol Management

Symbols are identified by `SymbolId` (`uint32_t`):

```cpp
SymbolRegistry registry;
registry.registerSymbol("binance", "BTCUSDT");  // Returns SymbolId
auto id = registry.getSymbolId("binance", "BTCUSDT");
```

Benefits:

- Fast comparison (integer vs string)
- Compact event structures
- Consistent across components

## Type System

FLOX uses strong types to prevent unit confusion:

| Type | Underlying | Purpose |
|------|------------|---------|
| `Price` | Fixed-point | Prices (avoid floating-point) |
| `Quantity` | Fixed-point | Quantities |
| `SymbolId` | `uint32_t` | Symbol identifier |
| `OrderId` | `uint64_t` | Order identifier |
| `UnixNanos` | `int64_t` | Nanosecond timestamp |

## Threading Model

```mermaid
flowchart TB
    subgraph main["Main Thread"]
        engine["Engine lifecycle<br/>Subsystem start/stop"]
    end

    subgraph connectors["Connector Threads"]
        c1["Connector 1<br/>Network I/O, Parsing"]
        c2["Connector 2<br/>Network I/O, Parsing"]
        c3["Connector N<br/>Network I/O, Parsing"]
    end

    subgraph consumers["Bus Consumer Threads"]
        s1["Strategy A"]
        s2["Strategy B"]
        agg["Aggregator"]
    end

    engine --> connectors
    engine --> consumers
    c1 --> s1
    c2 --> s2
    c3 --> agg
```

- Each connector manages its own threads
- Each bus consumer gets a dedicated thread
- Consumer threads can be pinned to isolated CPU cores

## CPU Affinity (Optional)

With `FLOX_ENABLE_CPU_AFFINITY=ON`:

```cpp
bus.setupOptimalConfiguration(EventBus::ComponentType::MARKET_DATA);
```

This:

- Pins consumer threads to isolated cores
- Sets real-time scheduling priority
- Enables NUMA-aware core assignment

See [Configure CPU Affinity](../how-to/cpu-affinity.md).

## Backtest realism stack

Backtesting reuses the same `Engine`, the same buses, and the same strategy class as live. What changes is the executor and the surrounding sim objects.

```mermaid
flowchart LR
    DATA[Tape / CSV / floxlog] --> EXE
    subgraph stack["VenueStack (one call)"]
        EXE[SimulatedExecutor]
        ACC[Cross-margin Account]
        LIQ[LiquidationEngine<br/>MM tiers + ADL]
        FEE[VIP fee schedule]
        FUND[Funding schedule]
        RL[Rate-limit policy]
        AVAIL[Venue-availability hook]
    end
    EXE --> ACC
    LIQ -. on_marks .-> ACC
    FEE -. records realized notional .-> ACC
    FUND -. settles on interval .-> ACC
    EXE --> RL
    EXE --> AVAIL
```

`flox.VenueStack.binance_um_futures(...)` (and the other factories) wires the whole stack in one call. The strategy class doesn't know the difference between this and a live exchange. See [Realistic backtest in one call](../how-to/realistic-backtest.md) and [Cross-margin accounts](../how-to/cross-margin.md).

The bare `BacktestRunner` skips everything except a flat fee — useful for indicator sanity checks, not for capital decisions.

## Execution paths (broker pattern)

One strategy class runs backtest, paper, and live. The piece that varies is the broker behind the signal callback:

```mermaid
flowchart LR
    STRAT[Your Strategy] --> SIG[Signal]
    SIG --> BR{Broker}
    BR -->|backtest| SIM[SimulatedExecutor<br/>+ VenueStack]
    BR -->|paper| PAPER[PaperBroker<br/>live feed -> SimulatedExecutor]
    BR -->|live| CCXT[CcxtBroker<br/>ccxt.pro -> exchange]
```

`PaperBroker` runs the same `SimulatedExecutor` used in the realistic backtest, but on a live feed. `CcxtBroker` routes the same signals through a real exchange. Switching execution paths is a constructor change, not a code rewrite. See [Paper trading](../how-to/paper-trading.md) and [Connect FLOX to a CCXT exchange](../how-to/ccxt-adapter.md).

## MCP control plane

FLOX ships an MCP server that exposes the engine to AI agents under scoped tokens. Read tokens see strategy state, decision logs, and event history. Paper tokens can also place orders against a `PaperBroker`. Live tokens can route real orders, but each one passes through an out-of-band approval step and is written to the audit log.

```mermaid
flowchart LR
    AI[AI agent] -->|MCP, scoped token| SRV[FLOX MCP server]
    SRV -->|read| STATE[Strategy state<br/>decisions, events]
    SRV -->|paper / live| BR{Broker}
    SRV -->|live order| OOB[Out-of-band approval]
    OOB --> BR
    SRV --> AUDIT[Audit log]
```

See [Control engine over MCP](../how-to/mcp-control-plane.md) and [MCP control plane](mcp-control-plane.md) for the design.

## Next Steps

- [The Disruptor Pattern](disruptor.md) — Deep dive into event delivery
- [Memory Model](memory-model.md) — Zero-allocation design
- [MCP control plane](mcp-control-plane.md) — scoped AI control over the engine
- [First Strategy](../tutorials/first-strategy.md) — Write your first strategy
