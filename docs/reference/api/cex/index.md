# Cross-Exchange Coordination (CEX)

Components for multi-exchange trading on centralized exchanges.

## Overview

The CEX module provides building blocks for:

- **Multi-exchange book aggregation** - CompositeBookMatrix provides atomic, thread-safe top-of-book across exchanges
- **Cross-exchange arbitrage detection** - Identify when bid on one exchange exceeds ask on another
- **Aggregated position tracking** - Track positions across exchanges with lock-free reads
- **Clock synchronization** - RTT-based offset estimation with EMA smoothing
- **Smart order routing** - Route orders based on best price, lowest latency, or custom strategies
- **Split order tracking** - Track parent-child order relationships

## Design Principles

- **Zero allocations** on hot paths
- **No `std::variant`** - 17-20% slower than tag+switch dispatch
- **No `std::expected`** - C++23, poor platform support
- **Fixed-size containers** where possible
- **Thread-safe atomic reads** for multi-consumer EventBus architecture

## Components

| Component | Description |
|-----------|-------------|
| [ExchangeInfo](exchange_info.md) | Exchange metadata with fixed-size name |
| [ExchangeClockSync](exchange_clock_sync.md) | RTT-based clock synchronization |
| [CompositeBookMatrix](composite_book_matrix.md) | Thread-safe multi-exchange order book |
| [AggregatedPositionTracker](aggregated_position_tracker.md) | Thread-safe position aggregation |
| [SplitOrderTracker](split_order_tracker.md) | Parent-child order tracking |
| [OrderRouter](order_router.md) | Smart order routing |

## Quick Start

```cpp
#include "flox/book/composite_book_matrix.h"
#include "flox/position/aggregated_position_tracker.h"
#include "flox/execution/order_router.h"
#include "flox/util/sync/exchange_clock_sync.h"

// 1. Set up exchanges
SymbolRegistry registry;
ExchangeId binance = registry.registerExchange("Binance");
ExchangeId bybit = registry.registerExchange("Bybit");

// 2. Register symbols with equivalence
SymbolId btcBinance = registry.registerSymbol(binance, "BTCUSDT");
SymbolId btcBybit = registry.registerSymbol(bybit, "BTCUSDT");
registry.mapEquivalentSymbols({btcBinance, btcBybit});

// 3. Set up composite book (thread-safe)
CompositeBookMatrix<4> matrix;
// ... subscribe to book updates from both exchanges

// 4. Check for arbitrage
if (matrix.hasArbitrageOpportunity(btcBinance)) {
    auto bid = matrix.bestBid(btcBinance);
    auto ask = matrix.bestAsk(btcBinance);
    // bid.exchange != ask.exchange and bid.priceRaw > ask.priceRaw
}

// 5. Track aggregated positions
AggregatedPositionTracker<4> positions;
auto total = positions.totalPosition(btcBinance);  // Lock-free read

// 6. Route orders
OrderRouter<4> router;
router.registerExecutor(binance, &binanceExecutor);
router.registerExecutor(bybit, &bybitExecutor);
router.setRoutingStrategy(RoutingStrategy::BestPrice);
router.route(btcBinance, Side::BUY, priceRaw, qtyRaw, orderId);
```

## Thread Safety Model

The CEX components are designed for the EventBus multi-consumer architecture where each consumer runs in its own thread:

```
BookBus Consumer Thread          Strategy Consumer Thread
        │                                  │
        ▼                                  ▼
CompositeBookMatrix.onBookUpdate()   CompositeBookMatrix.bestBid()
        │                                  │
        ▼                                  ▼
   Atomic store (release)            Atomic load (acquire)
```

| Component | Writer Thread | Reader Thread | Mechanism |
|-----------|---------------|---------------|----------|
| CompositeBookMatrix | BookBus consumer | Strategy consumer | Atomic top-of-book |
| AggregatedPositionTracker | ExecutionBus consumer | Strategy consumer | Atomic position snapshot |
| ExchangeClockSync | Connector thread | OrderRouter | Single writer, atomic reads |
| SplitOrderTracker | Single thread only | Single thread only | No atomics needed |
| OrderRouter | N/A (stateless routing) | Strategy thread | Reads atomic data from above |

## Performance

Benchmark results (Intel Core i7, Release build):

| Operation | Latency | Throughput |
|-----------|---------|------------|
| CompositeBookMatrix.bestBid() (4 exchanges) | ~6ns | 160M/s |
| CompositeBookMatrix.update() | ~5ns | 218M/s |
| PositionTracker.position() (single exchange) | ~3ns | 390M/s |
| PositionTracker.totalPosition() (8 exchanges) | ~7ns | 144M/s |
| ClockSync.toLocalTimeNs() | <1ns | 10G/s |
| Atomic load (baseline) | ~0.1ns | 7.8G/s |

## Demo

Run the CEX demo to see all components in action:

```bash
./build/external/flox-connectors/external/flox/demo/cex_demo
```

The demo demonstrates:
1. Exchange registration and symbol equivalence
2. Clock synchronization with multiple exchanges
3. Composite order book with arbitrage detection
4. Aggregated position tracking
5. Smart order routing with failover
6. Split order tracking
7. Arbitrage detection and execution
