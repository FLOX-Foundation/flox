# Multi-Exchange Trading

Set up trading across multiple exchanges with position aggregation and smart routing.

## Prerequisites

- Multiple exchange connectors configured
- SymbolRegistry with registered exchanges

## 1. Register Exchanges and Symbols

```cpp
#include "flox/engine/symbol_registry.h"

SymbolRegistry registry;

// Register exchanges
ExchangeId binance = registry.registerExchange("Binance");
ExchangeId bybit = registry.registerExchange("Bybit");
ExchangeId kraken = registry.registerExchange("Kraken");

// Register symbols per exchange
SymbolId btcBinance = registry.registerSymbol(binance, "BTCUSDT");
SymbolId btcBybit = registry.registerSymbol(bybit, "BTCUSDT");
SymbolId btcKraken = registry.registerSymbol(kraken, "XBTUSDT");

// Map equivalent symbols
std::array<SymbolId, 3> btcSymbols = {btcBinance, btcBybit, btcKraken};
registry.mapEquivalentSymbols(btcSymbols);
```

## 2. Set Up Clock Synchronization

```cpp
#include "flox/util/sync/exchange_clock_sync.h"

ExchangeClockSync<8> clockSync;

// Record timing samples from API calls
void onApiResponse(ExchangeId ex, int64_t localSend, int64_t serverTime, int64_t localRecv)
{
  clockSync.recordSample(ex, localSend, serverTime, localRecv);
}

// Convert exchange timestamp to local time
int64_t localTs = clockSync.toLocalTimeNs(binance, exchangeTimestamp);

// Check sync quality
if (clockSync.hasReliableSync(binance)) {
  auto est = clockSync.estimate(binance);
  // est.offsetNs, est.latencyNs, est.confidenceNs
}
```

## 3. Aggregate Order Books

```cpp
#include "flox/book/composite_book_matrix.h"

CompositeBookMatrix<4> books;

// Subscribe to BookUpdateEvents from all exchanges
// The matrix updates atomically on each event

// Query best prices (thread-safe, lock-free)
auto bestBid = books.bestBid(btcBinance);  // Best bid across all exchanges
auto bestAsk = books.bestAsk(btcBinance);  // Best ask across all exchanges

if (bestBid.valid && bestAsk.valid) {
  int64_t spread = bestAsk.priceRaw - bestBid.priceRaw;
  // bestBid.exchange, bestAsk.exchange tell you where
}

// Check for arbitrage
if (books.hasArbitrageOpportunity(btcBinance)) {
  // bestBid.priceRaw > bestAsk.priceRaw across different exchanges
}
```

## 4. Track Positions

```cpp
#include "flox/position/aggregated_position_tracker.h"

AggregatedPositionTracker<8> positions;

// Update on fills (writer thread)
positions.onFill(binance, btcBinance, 100'000'000LL, 50000'000'000LL);  // Buy 100 @ 50000
positions.onFill(bybit, btcBybit, -30'000'000LL, 50100'000'000LL);      // Sell 30 @ 50100

// Query positions (reader thread, lock-free)
auto binancePos = positions.position(binance, btcBinance);
auto totalPos = positions.totalPosition(btcBinance);  // Aggregated across exchanges

int64_t pnl = positions.unrealizedPnlRaw(btcBinance, currentPriceRaw);
```

## 5. Configure Order Routing

```cpp
#include "flox/execution/order_router.h"

OrderRouter<4> router;

// Register executors
router.registerExecutor(binance, &binanceExecutor);
router.registerExecutor(bybit, &bybitExecutor);
router.registerExecutor(kraken, &krakenExecutor);

// Configure strategy
router.setCompositeBook(&books);
router.setClockSync(&clockSync);
router.setRoutingStrategy(RoutingStrategy::BestPrice);
router.setFailoverPolicy(FailoverPolicy::FailoverToBest);

// Route orders
ExchangeId routed;
auto err = router.route(btcBinance, Side::BUY, priceRaw, qtyRaw, orderId, &routed);

if (err == RoutingError::Success) {
  // Order sent to 'routed' exchange
}

// Or route explicitly
router.routeTo(binance, btcBinance, Side::BUY, priceRaw, qtyRaw, orderId);
```

## 6. Track Split Orders

```cpp
#include "flox/execution/split_order_tracker.h"

SplitOrderTracker tracker;

// Split parent order across exchanges
OrderId parentId = 1000;
std::array<OrderId, 3> childIds = {1001, 1002, 1003};
int64_t totalQty = 1000'000'000LL;

tracker.registerSplit(parentId, childIds, totalQty, nowNs);

// Update on child events
tracker.onChildFill(1001, 400'000'000LL);
tracker.onChildComplete(1001, true);

// Check status
auto* state = tracker.getState(parentId);
double fillRatio = state->fillRatio();  // 0.4
bool done = state->allDone();
```

## Routing Strategies

| Strategy | Description | Use Case |
|----------|-------------|----------|
| `BestPrice` | Route to exchange with best price | Default, maximize fill price |
| `LowestLatency` | Route to exchange with lowest RTT | Time-sensitive orders |
| `LargestSize` | Route to exchange with most liquidity | Large orders |
| `RoundRobin` | Distribute evenly | Load balancing |
| `Explicit` | Use order's target exchange | Manual control |

## Error Handling

```cpp
auto err = router.route(symbol, side, price, qty, orderId);

switch (err) {
  case RoutingError::Success:
    break;
  case RoutingError::NoExecutor:
    // No executor registered or all disabled
    break;
  case RoutingError::ExchangeDisabled:
    // Target exchange temporarily disabled
    break;
  case RoutingError::InvalidSymbol:
    // Symbol not found
    break;
}
```

## See Also

- [CEX Components](../components/cex/index.md)
- [Custom Connector](custom-connector.md)
- [Backtest Guide](backtest.md)
