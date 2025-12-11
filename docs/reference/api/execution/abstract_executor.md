# IOrderExecutor

`IOrderExecutor` defines the interface for components responsible for submitting, canceling, and replacing orders. It acts as the execution gateway in both simulated and live environments.

```cpp
class IOrderExecutor : public ISubsystem {
public:
  virtual ~IOrderExecutor() = default;

  virtual void submitOrder(const Order& order) {};
  virtual void cancelOrder(OrderId orderId) {};
  virtual void replaceOrder(OrderId oldOrderId, const Order& newOrder) {};
};
```

## Purpose

* Abstract execution interface used by strategies and internal components to place and manage orders.

## Responsibilities

| Method         | Description                                            |
| -------------- | ------------------------------------------------------ |
| `submitOrder`  | Sends a new order to the execution venue or simulator. |
| `cancelOrder`  | Cancels a previously submitted order.                  |
| `replaceOrder` | Replaces an existing order with new parameters.        |

## Notes

* Implements `ISubsystem`, enabling lifecycle coordination via `start()` and `stop()`.
* Can be backed by mocks, simulators, or real exchange adapters.
* Actual routing and fill simulation logic resides in concrete subclasses.
