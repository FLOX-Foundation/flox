# IOrderExecutor

`IOrderExecutor` defines the interface for components responsible for submitting, canceling, and replacing orders. It acts as the execution gateway in both simulated and live environments.

```cpp
struct OCOParams
{
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

## Order Management

| Method           | Description                                            |
| ---------------- | ------------------------------------------------------ |
| `submitOrder`    | Sends a new order to the execution venue or simulator. |
| `cancelOrder`    | Cancels a previously submitted order by ID.            |
| `cancelAllOrders`| Cancels all pending orders for a given symbol.         |
| `replaceOrder`   | Replaces an existing order with new parameters.        |

## OCO Orders

```cpp
// Submit two linked orders - when one fills/cancels, the other is auto-canceled
OCOParams params{
  .order1 = takeProfitOrder,
  .order2 = stopLossOrder
};
executor->submitOCO(params);
```

| Method     | Description                                                   |
| ---------- | ------------------------------------------------------------- |
| `submitOCO`| Submits two linked orders (one-cancels-other).                |

## Capability Discovery

```cpp
auto caps = executor->capabilities();
if (caps.supportsOCO()) {
  executor->submitOCO(params);
} else {
  // Handle OCO manually
}
```

| Method        | Description                                           |
| ------------- | ----------------------------------------------------- |
| `capabilities`| Returns `ExchangeCapabilities` describing what the executor supports. |

See [ExchangeCapabilities](exchange_capabilities.md) for full capability list.

## Notes

* Implements `ISubsystem`, enabling lifecycle coordination via `start()` and `stop()`.
* Can be backed by mocks, simulators, or real exchange adapters.
* Actual routing and fill simulation logic resides in concrete subclasses.
* All methods have default empty implementations for optional overriding.
