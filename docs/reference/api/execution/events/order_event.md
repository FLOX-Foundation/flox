# OrderEvent

`OrderEvent` encapsulates a single order lifecycle transition and delivers it to components via `OrderExecutionBus`.

```cpp
enum class OrderEventStatus {
  NEW,
  SUBMITTED,
  ACCEPTED,
  PARTIALLY_FILLED,
  FILLED,
  PENDING_CANCEL,
  CANCELED,
  EXPIRED,
  REJECTED,
  REPLACED,
  // Conditional order statuses
  PENDING_TRIGGER,
  TRIGGERED,
  TRAILING_UPDATED
};

struct OrderEvent {
  using Listener = IOrderExecutionListener;

  OrderEventStatus status{};
  Order order{};
  Order newOrder{};
  Quantity fillQty{0};
  std::string rejectReason;

  // For fills and trailing updates
  Price fillPrice{};
  Price newTrailingPrice{};

  uint64_t tickSequence{0};
  uint64_t recvNs{0};
  uint64_t publishNs{0};
  int64_t exchangeTsNs{0};

  void dispatchTo(IOrderExecutionListener& listener) const;
};
```

## Purpose

* Represent and route order state changes (submission, fills, cancelation, etc.) to execution listeners.

## Core Fields

| Field        | Description                                                 |
|--------------|-------------------------------------------------------------|
| status       | Event type — one of the `OrderEventStatus` values.          |
| order        | The primary order involved in the event.                    |
| newOrder     | Used only for `REPLACED` events.                            |
| fillQty      | Quantity filled (used in `PARTIALLY_FILLED` and `FILLED`).  |
| rejectReason | Human-readable rejection reason (for `REJECTED` events).    |
| tickSequence | Event ordering marker for sequencing and backtesting.       |

## Advanced Fields

| Field            | Description                                              |
|------------------|----------------------------------------------------------|
| fillPrice        | Execution price for filled orders.                       |
| newTrailingPrice | Updated trigger price for `TRAILING_UPDATED` events.     |
| recvNs           | Receive timestamp (nanoseconds).                         |
| publishNs        | Publish timestamp (nanoseconds).                         |
| exchangeTsNs     | Exchange timestamp (nanoseconds).                        |

## OrderEventStatus

| Status           | Description                                              |
|------------------|----------------------------------------------------------|
| `NEW`            | Order created but not yet submitted.                     |
| `SUBMITTED`      | Order sent to exchange.                                  |
| `ACCEPTED`       | Exchange acknowledged the order.                         |
| `PARTIALLY_FILLED` | Order partially executed.                              |
| `FILLED`         | Order fully executed.                                    |
| `PENDING_CANCEL` | Cancel request sent.                                     |
| `CANCELED`       | Order canceled.                                          |
| `EXPIRED`        | Order expired (GTD/IOC).                                 |
| `REJECTED`       | Order rejected by exchange.                              |
| `REPLACED`       | Order modified (price/quantity change).                  |
| `PENDING_TRIGGER` | Conditional order waiting for trigger condition.        |
| `TRIGGERED`      | Conditional order trigger condition met.                 |
| `TRAILING_UPDATED` | Trailing stop trigger price updated.                   |

## Dispatch Logic

```cpp
void dispatchTo(IOrderExecutionListener& listener) const;
```

Routes the event to the appropriate method:

| Status             | Dispatched Method                            |
|--------------------|----------------------------------------------|
| `SUBMITTED`        | `onOrderSubmitted(order)`                    |
| `ACCEPTED`         | `onOrderAccepted(order)`                     |
| `PARTIALLY_FILLED` | `onOrderPartiallyFilled(order, fillQty)`     |
| `FILLED`           | `onOrderFilled(order)`                       |
| `PENDING_CANCEL`   | `onOrderPendingCancel(order)`                |
| `CANCELED`         | `onOrderCanceled(order)`                     |
| `EXPIRED`          | `onOrderExpired(order)`                      |
| `REJECTED`         | `onOrderRejected(order, rejectReason)`       |
| `REPLACED`         | `onOrderReplaced(order, newOrder)`           |
| `PENDING_TRIGGER`  | `onOrderPendingTrigger(order)`               |
| `TRIGGERED`        | `onOrderTriggered(order)`                    |
| `TRAILING_UPDATED` | `onTrailingStopUpdated(order, newTrailingPrice)` |

## Notes

* Dispatch is type-safe and static — no RTTI or dynamic casts.
* `tickSequence` ensures global ordering consistency across mixed event streams.
* Used by `EventBus<OrderEvent, *>` and delivered to `IOrderExecutionListener` implementations.
* Conditional order statuses (`PENDING_TRIGGER`, `TRIGGERED`, `TRAILING_UPDATED`) are emitted by `SimulatedExecutor` during backtest.

## See Also

* [Order](../order.md) — Order structure
* [IOrderExecutionListener](../abstract_execution_listener.md) — Event listener interface
* [OrderExecutionBus](../bus/order_execution_bus.md) — Event bus
