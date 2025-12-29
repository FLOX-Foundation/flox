# OrderRouter

Smart order routing across multiple exchanges.

## Header

```cpp
#include "flox/execution/order_router.h"
```

## Synopsis

```cpp
enum class RoutingStrategy : uint8_t {
  BestPrice,      // Route to exchange with best price
  LowestLatency,  // Route to exchange with lowest latency
  LargestSize,    // Route to exchange with most liquidity
  RoundRobin,     // Cycle through available exchanges
  Explicit        // Use targetExchange field in order
};

enum class FailoverPolicy : uint8_t {
  Reject,           // Reject if target unavailable
  FailoverToBest,   // Failover to best available
  Notify            // Notify via callback
};

enum class RoutingError : uint8_t {
  Success = 0,
  NoExecutor,
  ExchangeDisabled,
  InvalidSymbol,
  RejectedByPolicy
};

template <size_t MaxExchanges = 4>
class OrderRouter : public ISubsystem
{
public:
  // Executor registration
  void registerExecutor(ExchangeId exchange, IOrderExecutor* executor) noexcept;
  void setEnabled(ExchangeId exchange, bool enabled) noexcept;

  // Configuration
  void setCompositeBook(CompositeBookMatrix<MaxExchanges>* book) noexcept;
  void setClockSync(ExchangeClockSync<MaxExchanges>* clockSync) noexcept;
  void setRoutingStrategy(RoutingStrategy strategy) noexcept;
  void setFailoverPolicy(FailoverPolicy policy) noexcept;

  // Routing
  RoutingError route(SymbolId symbol, Side side, int64_t priceRaw,
                     int64_t quantityRaw, OrderId orderId,
                     ExchangeId* outExchange = nullptr) noexcept;

  // Explicit routing
  RoutingError routeTo(ExchangeId exchange, SymbolId symbol, Side side,
                       int64_t priceRaw, int64_t quantityRaw,
                       OrderId orderId) noexcept;

  // Cancel
  RoutingError cancelOn(ExchangeId exchange, OrderId orderId) noexcept;

  // Exchange selection (analysis only)
  ExchangeId selectExchange(SymbolId symbol, Side side) const noexcept;
};
```

## Routing Strategies

### BestPrice

Routes to the exchange with the best price for the order:
- **BUY orders**: Route to exchange with lowest ask
- **SELL orders**: Route to exchange with highest bid

Requires `setCompositeBook()` to be called.

```cpp
router.setCompositeBook(&matrix);
router.setRoutingStrategy(RoutingStrategy::BestPrice);
router.route(symbol, Side::BUY, priceRaw, qtyRaw, orderId);
```

### LowestLatency

Routes to the exchange with the lowest measured latency.

Requires `setClockSync()` to be called.

```cpp
router.setClockSync(&clockSync);
router.setRoutingStrategy(RoutingStrategy::LowestLatency);
router.route(symbol, Side::BUY, priceRaw, qtyRaw, orderId);
```

### RoundRobin

Cycles through available exchanges in sequence.

```cpp
router.setRoutingStrategy(RoutingStrategy::RoundRobin);
// First order goes to exchange 0, second to exchange 1, etc.
```

### Explicit

Uses the `routeTo()` method to explicitly specify the target exchange.

```cpp
router.routeTo(exchangeId, symbol, side, priceRaw, qtyRaw, orderId);
```

## Failover Policies

### Reject (Default)

Returns `RoutingError::NoExecutor` or `RoutingError::ExchangeDisabled` if the target exchange is unavailable.

### FailoverToBest

If the target exchange is unavailable, routes to the best available exchange.

```cpp
router.setFailoverPolicy(FailoverPolicy::FailoverToBest);
router.setEnabled(0, false);  // Disable exchange 0

ExchangeId routedTo;
auto err = router.route(symbol, side, priceRaw, qtyRaw, orderId, &routedTo);
// err == Success, routedTo is next best exchange
```

## Usage

### Basic Setup

```cpp
OrderRouter<4> router;

// Register executors
router.registerExecutor(0, &binanceExecutor);
router.registerExecutor(1, &bybitExecutor);
router.registerExecutor(2, &krakenExecutor);

// Configure routing
router.setRoutingStrategy(RoutingStrategy::BestPrice);
router.setFailoverPolicy(FailoverPolicy::FailoverToBest);
router.setCompositeBook(&matrix);
router.setClockSync(&clockSync);
```

### Routing Orders

```cpp
ExchangeId routedTo;
auto err = router.route(symbol, Side::BUY, priceRaw, qtyRaw, orderId, &routedTo);

if (err == RoutingError::Success) {
  std::cout << "Routed to exchange " << routedTo << "\n";
} else if (err == RoutingError::NoExecutor) {
  std::cout << "No executor available\n";
} else if (err == RoutingError::ExchangeDisabled) {
  std::cout << "Exchange disabled\n";
}
```

### Dynamic Exchange Management

```cpp
// Disable an exchange (e.g., on disconnect)
router.setEnabled(exchangeId, false);

// Re-enable when reconnected
router.setEnabled(exchangeId, true);
```

## Error Handling

All routing methods return `RoutingError`:

| Error | Description |
|-------|-------------|
| `Success` | Order successfully routed |
| `NoExecutor` | No executor registered for selected exchange |
| `ExchangeDisabled` | Exchange is disabled via `setEnabled(false)` |
| `InvalidSymbol` | Symbol not recognized |
| `RejectedByPolicy` | Rejected by failover policy |

## Performance

| Operation | Complexity |
|-----------|------------|
| route() with BestPrice | O(MaxExchanges) |
| route() with LowestLatency | O(MaxExchanges) |
| route() with RoundRobin | O(MaxExchanges) worst case |
| routeTo() | O(1) |
| selectExchange() | O(MaxExchanges) |

No allocations in any routing path.

## See Also

- [CompositeBookMatrix](composite_book_matrix.md) - Multi-exchange book for BestPrice routing
- [ExchangeClockSync](exchange_clock_sync.md) - Latency measurement for LowestLatency routing
- [SplitOrderTracker](split_order_tracker.md) - Track split orders across exchanges
