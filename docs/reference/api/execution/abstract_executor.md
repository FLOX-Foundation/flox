# IOrderExecutor

`IOrderExecutor` defines the interface for components responsible for submitting, canceling, and replacing orders. It acts as the execution gateway in both simulated and live environments.

```cpp
struct OCOParams {
  Order order1;
  Order order2;
};

class IOrderExecutor : public ISubsystem {
public:
  virtual ~IOrderExecutor() = default;

  virtual void submitOrder(const Order& order) {}
  virtual void cancelOrder(OrderId orderId) {}
  virtual void cancelAllOrders(SymbolId symbol) {}
  virtual void replaceOrder(OrderId oldOrderId, const Order& newOrder) {}

  // OCO: one-cancels-other
  virtual void submitOCO(const OCOParams& params) {}

  // Capability discovery
  virtual ExchangeCapabilities capabilities() const { return ExchangeCapabilities::simulated(); }
};
```

## Purpose

* Abstract execution interface used by strategies and internal components to place and manage orders.

## Basic Methods

| Method           | Description                                            |
|------------------|--------------------------------------------------------|
| `submitOrder`    | Sends a new order to the execution venue or simulator. |
| `cancelOrder`    | Cancels a previously submitted order.                  |
| `cancelAllOrders`| Cancels all orders for a symbol.                       |
| `replaceOrder`   | Replaces an existing order with new parameters.        |

## OCO (One-Cancels-Other)

```cpp
virtual void submitOCO(const OCOParams& params);
```

Submits two linked orders. When one fills or is canceled, the other is automatically canceled.

### OCOParams

| Field  | Description        |
|--------|--------------------|
| order1 | First order.       |
| order2 | Second order.      |

### OCO Lifecycle

1. Both orders submitted
2. When either fills → the other is canceled

### Example: Breakout OCO

```cpp
Order buyStop;
buyStop.id = 1;
buyStop.symbol = symbolId;
buyStop.side = Side::BUY;
buyStop.type = OrderType::STOP_MARKET;
buyStop.triggerPrice = Price::fromDouble(105.0);
buyStop.quantity = Quantity::fromDouble(1.0);

Order sellStop;
sellStop.id = 2;
sellStop.symbol = symbolId;
sellStop.side = Side::SELL;
sellStop.type = OrderType::STOP_MARKET;
sellStop.triggerPrice = Price::fromDouble(95.0);
sellStop.quantity = Quantity::fromDouble(1.0);

OCOParams params;
params.order1 = buyStop;
params.order2 = sellStop;

executor->submitOCO(params);
// When price breaks 105 → buy triggers, sell canceled
// When price breaks 95 → sell triggers, buy canceled
```

## Capability Discovery

```cpp
virtual ExchangeCapabilities capabilities() const;
```

Returns the capabilities of the execution venue. Strategies should check capabilities before using advanced features:

```cpp
auto caps = executor->capabilities();
if (caps.supports(OrderType::TRAILING_STOP)) {
  // Safe to use trailing stop
}
```

## Notes

* Implements `ISubsystem`, enabling lifecycle coordination via `start()` and `stop()`.
* Can be backed by mocks, simulators, or real exchange adapters.
* Actual routing and fill simulation logic resides in concrete subclasses.
* Default implementations are no-ops.

## See Also

* [ExchangeCapabilities](exchange_capabilities.md) — Feature discovery
* [Order](order.md) — Order structure
* [SimulatedExecutor](../../backtest/simulated_executor.md) — Backtest implementation
