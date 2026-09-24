# OrderTracker

`OrderTracker` is a thread-safe container for tracking order state throughout the order lifecycle. It provides mutex-protected access to order status, fills, and exchange metadata, bounded by a configurable capacity.

```cpp
struct OrderState {
  Order localOrder;
  std::string exchangeOrderId;
  std::string clientOrderId;
  OrderEventStatus status{OrderEventStatus::NEW};
  Quantity filled{};
  TimePoint createdAt{};
  TimePoint lastUpdate{};

  bool isTerminal() const noexcept;
};

class OrderTracker {
public:
  OrderTracker();                          // capacity = config::ORDER_TRACKER_CAPACITY
  explicit OrderTracker(size_t capacity);

  size_t capacity() const noexcept;

  bool onSubmitted(const Order& order, std::string_view exchangeOrderId,
                   std::string_view clientOrderId = "");
  bool onFilled(OrderId id, Quantity fill);
  bool onPendingCancel(OrderId id);
  bool onCanceled(OrderId id);
  bool onRejected(OrderId id, std::string_view reason);
  bool onReplaced(OrderId oldId, const Order& newOrder,
                  std::string_view newExchangeId, std::string_view newClientOrderId = "");

  std::optional<OrderState> get(OrderId id) const;
  bool exists(OrderId id) const;
  bool isActive(OrderId id) const;
  std::optional<OrderEventStatus> getStatus(OrderId id) const;

  size_t activeOrderCount() const;
  size_t totalOrderCount() const;
  void pruneTerminal();
};
```

## Purpose

* Track order lifecycle from submission to completion.
* Provide thread-safe access to order state from multiple components.
* Map between local `OrderId`, exchange order IDs, and client order IDs.
* Handle edge cases gracefully (double cancel, duplicate IDs, etc.).

## Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `onSubmitted(order, exchangeId, clientId)` | `bool` | Record new order. Returns `false` if OrderId already exists, or if the tracker is full of live orders. |
| `onFilled(id, fill)` | `bool` | Update filled quantity. Returns `false` if order not found or terminal. |
| `onPendingCancel(id)` | `bool` | Mark as pending cancel. Returns `false` if order not found or terminal. |
| `onCanceled(id)` | `bool` | Mark as canceled. Returns `false` if already terminal (safe double-cancel). |
| `onRejected(id, reason)` | `bool` | Mark as rejected. Returns `false` if already terminal. |
| `onReplaced(oldId, newOrder, ...)` | `bool` | Handle order amendment. Marks old as REPLACED (terminal), inserts new. Returns `false` if the tracker is full of live orders. |
| `get(id)` | `optional<OrderState>` | Retrieve order state copy (nullopt if not found). |
| `exists(id)` | `bool` | Check if order exists. |
| `isActive(id)` | `bool` | Check if order exists and is not terminal. |
| `getStatus(id)` | `optional<Status>` | Get just the status without copying full state. |
| `activeOrderCount()` | `size_t` | Count of non-terminal orders. |
| `totalOrderCount()` | `size_t` | Total orders in tracker. |
| `pruneTerminal()` | `void` | Remove all terminal orders to free memory. Also called by the tracker itself when an insert hits the capacity bound. |
| `capacity()` | `size_t` | Maximum number of entries held. |

## OrderState Fields

| Field | Type | Description |
|-------|------|-------------|
| `localOrder` | `Order` | The original order structure. |
| `exchangeOrderId` | `std::string` | Exchange-assigned order ID. |
| `clientOrderId` | `std::string` | Client-assigned order ID (optional). |
| `status` | `OrderEventStatus` | Current order status. |
| `filled` | `Quantity` | Total quantity filled. |
| `createdAt` | `TimePoint` | When order was submitted. |
| `lastUpdate` | `TimePoint` | Last state update timestamp. |

## Terminal States

An order is considered terminal when status is one of:

- `FILLED` — fully executed
- `CANCELED` — canceled by user or system
- `REJECTED` — rejected by exchange
- `EXPIRED` — time-in-force expired
- `REPLACED` — superseded by an amendment; `onReplaced()` writes it on the old order, which will never report again

Terminal orders cannot be modified. Methods return `false` when attempting to modify terminal orders.

## Thread Safety

* All methods are protected by `std::mutex`.
* Safe for concurrent access from multiple threads.
* `get()` returns a copy to avoid holding locks during processing.

## Memory Management

* Bounded: the tracker holds at most `capacity()` entries, defaulting to
  `config::ORDER_TRACKER_CAPACITY` (4096, overridable through
  `FLOX_DEFAULT_ORDER_TRACKER_CAPACITY`). The map is sized once at
  construction, so the bound is a memory bound too.
* When an insert finds the map full, terminal entries are dropped to make
  room. History is what gets sacrificed — a live order is never evicted.
* If the map is full of *live* orders, the insert is refused:
  `onSubmitted()` / `onReplaced()` return `false` and log an error. Silently
  dropping a live order would leave a working order on the venue that the
  process no longer knows about, which is strictly worse than refusing the
  submit where the caller can still react.
* `pruneTerminal()` remains callable directly to reclaim the memory sooner,
  for example at the end of a trading session.

## Example Usage

```cpp
OrderTracker tracker;

// Submit order
Order order{.id = 1, .symbol = 100, .side = Side::BUY, ...};
if (!tracker.onSubmitted(order, "EX123", "CLIENT456")) {
  // Duplicate order ID - handle error
}

// Check state
if (auto state = tracker.get(order.id)) {
  std::cout << "Exchange ID: " << state->exchangeOrderId << "\n";
  std::cout << "Status: " << static_cast<int>(state->status) << "\n";
}

// Safe double-cancel
tracker.onCanceled(order.id);  // returns true
tracker.onCanceled(order.id);  // returns false, no error

// Cleanup
tracker.pruneTerminal();
```

## See Also

* [Order](order.md) — Order structure definition
* [OrderEvent](events/order_event.md) — Order event for bus delivery
* [IExecutor](abstract_executor.md) — Executor interface
