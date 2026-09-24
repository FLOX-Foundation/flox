# OrderRouter

Smart order routing across multiple exchanges.

## Header

```cpp
#include "flox/execution/order_router.h"
```

!!! note "`IRoutableExecutor` is not `IOrderExecutor`"
    `execution/order_router.h` declares its own narrow order sink,
    `flox::IRoutableExecutor`:

    ```cpp
    virtual void submit(SymbolId, Side, Price, Quantity, OrderId) = 0;
    virtual void cancel(OrderId) = 0;
    ```

    `execution/abstract_executor.h` declares the full executor interface,
    `flox::IOrderExecutor` (`submitOrder`, `cancelOrder`, `cancelAllOrders`,
    `replaceOrder`, `submitOCO`, `capabilities`), which is what
    `SimulatedExecutor` implements.

    They are different interfaces with different names, so the two headers
    coexist in one translation unit. `registerExecutor()` takes
    `IRoutableExecutor*`: passing an `IOrderExecutor` (for example
    `SimulatedExecutor`) is a type error, and a venue executor reaches the
    router through a small adapter.

## Synopsis

```cpp
enum class RoutingStrategy : uint8_t {
  BestPrice,      // Route to exchange with best price
  LowestLatency,  // Route to exchange with lowest latency
  LargestSize,    // Route to exchange with most liquidity
  RoundRobin      // Cycle through available exchanges
  // No Explicit strategy: route() carries no per-order exchange. To target a
  // specific venue use routeTo(), which is the explicit API.
};

enum class FailoverPolicy : uint8_t {
  Reject,         // Reject if target unavailable
  FailoverToBest  // Failover to best available
  // No Notify policy: there is no callback member to notify through.
};

enum class RoutingError : uint8_t {
  Success = 0,
  NoExecutor,
  ExchangeDisabled
};

// The router's own narrow order sink. Deliberately NOT the
// flox::IOrderExecutor of execution/abstract_executor.h: that is the full
// executor interface, and two classes with the same fully-qualified name in
// one program is an ODR violation, so this one carries a distinct name.
class IRoutableExecutor
{
public:
  virtual ~IRoutableExecutor() = default;

  // Price and Quantity, not two int64_t raws: a swapped pair on the order
  // path is unrecoverable, so it is a compile error instead.
  virtual void submit(SymbolId symbol, Side side, Price price,
                      Quantity quantity, OrderId orderId) = 0;
  virtual void cancel(OrderId orderId) = 0;
};

template <size_t MaxExchanges = 4>
class OrderRouter : public ISubsystem
{
public:
  // Executor registration
  void registerExecutor(ExchangeId exchange, IRoutableExecutor* executor);
  void setEnabled(ExchangeId exchange, bool enabled);
  bool isEnabled(ExchangeId exchange) const;

  // Configuration
  void setCompositeBook(CompositeBookMatrix<MaxExchanges>* book);
  void setClockSync(ExchangeClockSync<MaxExchanges>* clockSync);
  void setRoutingStrategy(RoutingStrategy strategy);
  void setFailoverPolicy(FailoverPolicy policy);

  // Routing
  RoutingError route(SymbolId symbol, Side side, Price price,
                     Quantity quantity, OrderId orderId,
                     ExchangeId* outExchange = nullptr);

  // Explicit routing
  RoutingError routeTo(ExchangeId exchange, SymbolId symbol, Side side,
                       Price price, Quantity quantity, OrderId orderId);

  // Cancel
  RoutingError cancelOn(ExchangeId exchange, OrderId orderId);

  // Exchange selection (analysis only)
  ExchangeId selectExchange(SymbolId symbol, Side side) const;

  size_t enabledCount() const;
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
router.route(symbol, Side::BUY, price, quantity, orderId);
```

### LowestLatency

Routes to the exchange with the lowest measured latency.

Requires `setClockSync()` to be called.

```cpp
router.setClockSync(&clockSync);
router.setRoutingStrategy(RoutingStrategy::LowestLatency);
router.route(symbol, Side::BUY, price, quantity, orderId);
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
router.routeTo(exchangeId, symbol, side, price, quantity, orderId);
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
auto err = router.route(symbol, side, price, quantity, orderId, &routedTo);
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
auto err = router.route(symbol, Side::BUY, price, quantity, orderId, &routedTo);

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

## Performance

| Operation | Complexity |
|-----------|------------|
| route() with BestPrice | O(MaxExchanges) |
| route() with LowestLatency | O(MaxExchanges) |
| route() with RoundRobin | O(MaxExchanges) worst case |
| routeTo() | O(1) |
| selectExchange() | O(MaxExchanges) |

No allocations in any routing path.

## Thread Safety

`route()` is on the order path and takes no lock. Every word it reads is an
atomic: the executor table and the enabled flags are published with release
stores by `registerExecutor()` / `setEnabled()` from a control thread and read
with acquire loads by the routing thread, the configuration words
(`_strategy`, `_failoverPolicy`, the book and clock-sync pointers) are relaxed,
and the round-robin cursor is a `fetch_add`, so two routing threads get their
own slot instead of read-modify-writing a shared counter.

Note what this does and does not buy: a `setEnabled(false)` concurrent with a
`route()` may still let that in-flight order through, because the decision was
already taken. The guarantee is that every route *after* the disable is
observable is refused, and that there is no data race.

## See Also

- [CompositeBookMatrix](composite_book_matrix.md) - Multi-exchange book for BestPrice routing
- [ExchangeClockSync](exchange_clock_sync.md) - Latency measurement for LowestLatency routing
- [SplitOrderTracker](split_order_tracker.md) - Track split orders across exchanges
