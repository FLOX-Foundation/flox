# OrderTracker

`OrderTracker` is a lock-free hash map for tracking order state throughout the order lifecycle. It provides thread-safe access to order status, fills, and exchange metadata.

```cpp
struct OrderState {
  Order localOrder;
  std::string exchangeOrderId;
  std::string clientOrderId;
  std::atomic<OrderEventStatus> status{OrderEventStatus::NEW};
  std::atomic<Quantity> filled = Quantity::fromDouble(0.0);
  TimePoint createdAt{};
  std::atomic<TimePoint> lastUpdate{};
};

class OrderTracker {
public:
  static constexpr std::size_t SIZE = config::ORDER_TRACKER_CAPACITY;

  OrderTracker();

  void onSubmitted(const Order& order, std::string_view exchangeOrderId,
                   std::string_view clientOrderId = "");
  void onFilled(OrderId id, Quantity fill);
  void onCanceled(OrderId id);
  void onRejected(OrderId id, std::string_view reason);
  void onReplaced(OrderId oldId, const Order& newOrder,
                  std::string_view newExchangeId, std::string_view newClientOrderId = "");

  const OrderState* get(OrderId id) const;
};
```

## Purpose

* Track order lifecycle from submission to completion.
* Provide thread-safe access to order state from multiple components.
* Map between local `OrderId`, exchange order IDs, and client order IDs.

## Methods

| Method | Description |
|--------|-------------|
| `onSubmitted(order, exchangeId, clientId)` | Record new order submission. |
| `onFilled(id, fill)` | Update filled quantity for an order. |
| `onCanceled(id)` | Mark order as canceled. |
| `onRejected(id, reason)` | Mark order as rejected with reason. |
| `onReplaced(oldId, newOrder, newExchangeId, newClientId)` | Handle order replacement (amend). |
| `get(id)` | Retrieve order state (returns `nullptr` if not found). |

## OrderState Fields

| Field | Type | Description |
|-------|------|-------------|
| `localOrder` | `Order` | The original order structure. |
| `exchangeOrderId` | `std::string` | Exchange-assigned order ID. |
| `clientOrderId` | `std::string` | Client-assigned order ID (optional). |
| `status` | `atomic<OrderEventStatus>` | Current order status. |
| `filled` | `atomic<Quantity>` | Total quantity filled. |
| `createdAt` | `TimePoint` | When order was submitted. |
| `lastUpdate` | `atomic<TimePoint>` | Last state update timestamp. |

## Configuration

The tracker capacity is configurable at compile time:

```cpp
// In engine_config.h
#ifndef FLOX_DEFAULT_ORDER_TRACKER_CAPACITY
#define FLOX_DEFAULT_ORDER_TRACKER_CAPACITY 4096
#endif

namespace config {
  inline constexpr int ORDER_TRACKER_CAPACITY = FLOX_DEFAULT_ORDER_TRACKER_CAPACITY;
}
```

## Internal Design

* Uses open-addressing hash map with fixed-size slot array.
* Atomic operations for status and quantity updates.
* No allocations after construction.
* O(1) average lookup and insert.

## Notes

* Designed for high-frequency order tracking without locks.
* Capacity should be sized for maximum concurrent orders.
* Old orders are not automatically evicted; manage capacity accordingly.
* Thread-safe for concurrent reads and updates to different orders.

## See Also

* [Order](order.md) — Order structure definition
* [OrderEvent](events/order_event.md) — Order event for bus delivery
* [IExecutor](abstract_executor.md) — Executor interface
* [EngineConfig](../engine/engine_config.md) — Capacity configuration
