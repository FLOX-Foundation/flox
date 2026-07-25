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
  TRAILING_UPDATED,
  // Backtest-only microstructure statuses
  QUEUE_POSITION_UPDATED,
  MARKET_POSITION_CHANGED,
  // Replace-in-flight states
  REPLACE_SUBMITTED,
  REPLACE_ACCEPTED,
  REPLACE_REJECTED,
  // Client-side rate-limit rejection
  REJECTED_RATE_LIMIT,
  // On-chain (DEX) lifecycle
  PENDING_ONCHAIN,
  REVERTED,
  REPLACED_GAS
};

// Categorical position of a resting limit order relative to the
// current top-of-book on its side.
enum class MarketPosition : uint8_t {
  Unknown = 0,
  Best,        // our level is best on our side
  BehindBest,  // there is a better level on our side
  MidSpread,   // our price is strictly between best bid and best ask
  LevelEmpty,  // only our orders remain at this level
  Crossed,     // our price crosses the opposite side
};

// Per-lifecycle-stage timestamps stamped when the corresponding status
// transition fires. Zero means "not reached yet". Engine time on
// backtests, exchange time on live.
struct OrderTimestamps {
  int64_t submittedAtNs{0};
  int64_t acceptedAtNs{0};
  int64_t firstFillAtNs{0};
  int64_t lastFillAtNs{0};
  int64_t canceledAtNs{0};
  int64_t rejectedAtNs{0};
  int64_t triggeredAtNs{0};
  int64_t expiredAtNs{0};
};

struct OrderEvent {
  using Listener = IOrderExecutionListener;

  OrderEventStatus status = OrderEventStatus::NEW;
  Order order{};
  Order newOrder{};
  Quantity fillQty{0};
  std::string rejectReason;

  // On-chain (DEX) metadata, filled by the connector
  std::string txHash;
  uint32_t confirmations{0};

  // For fills and trailing updates
  Price fillPrice{};
  Price newTrailingPrice{};

  bool isMaker{false};

  Quantity queueAhead{0};
  Quantity queueTotal{0};

  MarketPosition marketPosition{MarketPosition::Unknown};
  int32_t distanceToBestTicks{0};

  OrderTimestamps timestamps{};

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
| isMaker          | For fills: `true` if the order rested and was consumed by an aggressive opposite trade; `false` if it arrived marketable. Meaningless for non-fill statuses. |
| queueAhead       | Volume in front of the order at its level. Set on `QUEUE_POSITION_UPDATED`, `PARTIALLY_FILLED`, `FILLED` for backtest limit orders. Zero on live events and non-limit orders. |
| queueTotal       | Total quantity at the order's level, same conditions as `queueAhead`. |
| marketPosition   | `MarketPosition` of a resting limit order relative to top-of-book. |
| distanceToBestTicks | Signed ticks from best on our side. Positive = behind best, negative = ahead (mid-spread / crossed). |
| timestamps       | `OrderTimestamps` snapshot at the moment the event was emitted. The slot for the current status is freshest; unreached stages read zero. |
| txHash           | On-chain transaction hash. Empty on CEX and backtest events.  |
| confirmations    | Blocks since inclusion. Zero on CEX and backtest events.       |
| recvNs           | Receive timestamp (nanoseconds).                         |
| publishNs        | Publish timestamp (nanoseconds).                         |
| exchangeTsNs     | Exchange timestamp (nanoseconds).                        |

`REVERTED` reuses `rejectReason` for the revert reason.

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
| `QUEUE_POSITION_UPDATED` | Queue position moved with no other lifecycle transition. Backtest only. |
| `MARKET_POSITION_CHANGED` | Resting order's categorical market position changed. Backtest only. |
| `REPLACE_SUBMITTED` | Fires immediately on `replaceOrder()`. Backtest only. |
| `REPLACE_ACCEPTED` | Fires after the ack latency on a successful replace. Backtest only. |
| `REPLACE_REJECTED` | The original order already filled or cannot be replaced. Backtest only. |
| `REJECTED_RATE_LIMIT` | Rejected pre-submission by client-side rate-limit enforcement. See `SimulatedExecutor::setRateLimitPolicy`. |
| `PENDING_ONCHAIN` | Broadcast to the mempool, not yet confirmed. Connector-driven. |
| `REVERTED`       | The chain rejected the transaction. Reason in `rejectReason`. Connector-driven. |
| `REPLACED_GAS`   | Re-broadcast with higher gas, superseding the pending tx. Connector-driven. |

`REPLACED` remains the terminal "old order gone, new order alive" status. The on-chain statuses are
never emitted by the backtest or CEX paths.

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
| `QUEUE_POSITION_UPDATED` | `onOrderQueuePositionChange(order, queueAhead, queueTotal)` |
| `MARKET_POSITION_CHANGED` | `onOrderMarketPositionChange(order, uint8_t(marketPosition), distanceToBestTicks)` |
| `REPLACE_SUBMITTED` | `onOrderReplaceSubmitted(order, newOrder)`   |
| `REPLACE_ACCEPTED` | `onOrderReplaceAccepted(order, newOrder)`    |
| `REPLACE_REJECTED` | `onOrderReplaceRejected(order, newOrder, rejectReason)` |
| `REJECTED_RATE_LIMIT` | `onOrderRejected(order, rejectReason)` — same handler as `REJECTED` |
| `PENDING_ONCHAIN`  | `onOrderPendingOnchain(order, txHash)`       |
| `REVERTED`         | `onOrderReverted(order, rejectReason)`       |
| `REPLACED_GAS`     | `onOrderGasReplaced(order, newOrder)`        |
| `NEW`              | Not dispatched.                              |

`IOrderExecutionListener::onOrderEvent(const OrderEvent&)` receives the raw event with every field
intact. It fires *after* the typed dispatch above.

## Notes

* Dispatch is type-safe and static — no RTTI or dynamic casts.
* `tickSequence` ensures global ordering consistency across mixed event streams.
* Used by `EventBus<OrderEvent, *>` and delivered to `IOrderExecutionListener` implementations.
* Conditional order statuses (`PENDING_TRIGGER`, `TRIGGERED`, `TRAILING_UPDATED`) are emitted by `SimulatedExecutor` during backtest.

## See Also

* [Order](../order.md) — Order structure
* [IOrderExecutionListener](../abstract_execution_listener.md) — Event listener interface
* [OrderExecutionBus](../bus/order_execution_bus.md) — Event bus
