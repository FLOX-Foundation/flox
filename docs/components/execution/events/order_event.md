# OrderEvent

`OrderEvent` encapsulates a single order lifecycle transition and delivers it to components via `OrderExecutionBus`.

```cpp
struct OrderEvent {
  using Listener = IOrderExecutionListener;

  OrderEventType type{};
  Order order{};
  Order newOrder{};
  Quantity fillQty{0};
  uint64_t tickSequence = 0;

  void dispatchTo(IOrderExecutionListener& listener) const;
};
```

## Purpose

* Represent and route order state changes (submission, fills, cancelation, etc.) to execution listeners.

## Responsibilities

| Field        | Description                                                 |
| ------------ | ----------------------------------------------------------- |
| type         | Event type — one of `SUBMITTED`, `FILLED`, `REPLACED`, etc. |
| order        | The primary order involved in the event.                    |
| newOrder     | Used only for `REPLACED` events.                            |
| fillQty      | Quantity filled (used only in `PARTIALLY_FILLED`).          |
| tickSequence | Event ordering marker for sequencing and backtesting.       |

## Dispatch Logic

```cpp
void dispatchTo(IOrderExecutionListener& listener) const;
```

Routes the event to the appropriate method:

| Type               | Dispatched Method                        |
| ------------------ | ---------------------------------------- |
| `SUBMITTED`        | `onOrderSubmitted(order)`                |
| `ACCEPTED`         | `onOrderAccepted(order)`                 |
| `PARTIALLY_FILLED` | `onOrderPartiallyFilled(order, fillQty)` |
| `FILLED`           | `onOrderFilled(order)`                   |
| `CANCELED`         | `onOrderCanceled(order)`                 |
| `EXPIRED`          | `onOrderExpired(order)`                  |
| `REJECTED`         | `onOrderRejected(order, /*reason*/ "")`  |
| `REPLACED`         | `onOrderReplaced(order, newOrder)`       |

## Notes

* Dispatch is type-safe and static — no RTTI or dynamic casts.
* `tickSequence` ensures global ordering consistency across mixed event streams.
* Used by `EventBus<OrderEvent, *>` and delivered to `IOrderExecutionListener` implementations.
