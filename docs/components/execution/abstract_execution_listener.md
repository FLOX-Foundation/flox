# IOrderExecutionListener

`IOrderExecutionListener` defines the interface for components that react to order lifecycle events. It is used by `OrderExecutionBus` to notify subscribers of changes in order state.

```cpp
class IOrderExecutionListener : public ISubscriber {
public:
  explicit IOrderExecutionListener(SubscriberId id);
  virtual ~IOrderExecutionListener() = default;

  SubscriberId id() const override;

  virtual void onOrderSubmitted(const Order& order) = 0;
  virtual void onOrderAccepted(const Order& order) = 0;
  virtual void onOrderPartiallyFilled(const Order& order, Quantity fillQty) = 0;
  virtual void onOrderFilled(const Order& order) = 0;
  virtual void onOrderCanceled(const Order& order) = 0;
  virtual void onOrderExpired(const Order& order) = 0;
  virtual void onOrderRejected(const Order& order, const std::string& reason) = 0;
  virtual void onOrderReplaced(const Order& oldOrder, const Order& newOrder) = 0;
};
```

## Purpose

* Provide a type-safe listener interface for receiving detailed updates on order status transitions.

## Responsibilities

| Method                   | Triggered On                                       |
| ------------------------ | -------------------------------------------------- |
| `onOrderSubmitted`       | Order submitted to venue or simulator.             |
| `onOrderAccepted`        | Order acknowledged/accepted by the exchange.       |
| `onOrderPartiallyFilled` | Partial fill received; includes fill quantity.     |
| `onOrderFilled`          | Fully filled.                                      |
| `onOrderCanceled`        | Canceled by user or system.                        |
| `onOrderExpired`         | Expired due to time-in-force or system conditions. |
| `onOrderRejected`        | Rejected by exchange or risk engine (with reason). |
| `onOrderReplaced`        | Order was replaced with a new one.                 |

## Notes

* Each listener is identified via a stable `SubscriberId`.
* Used in tandem with `OrderEvent::dispatchTo()` to decouple producers from listeners.
* Implemented by components such as `PositionManager`, `ExecutionTracker`, and metrics/reporting modules.
