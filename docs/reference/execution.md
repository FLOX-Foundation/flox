# Execution Reference

Orders, executors, and listeners.

## Order

**Header:** `flox/execution/order.h`

Represents a trading order.

```cpp
struct Order
{
  OrderId id{};
  Side side{};                           // BUY or SELL
  Price price{};
  Quantity quantity{};
  OrderType type{};                      // LIMIT or MARKET
  SymbolId symbol{};

  Quantity filledQuantity{0};

  TimePoint createdAt{};
  std::optional<TimePoint> lastUpdated{};
  std::optional<TimePoint> expiresAfter{};
  std::optional<TimePoint> exchangeTimestamp{};
};
```

---

## OrderEvent

**Header:** `flox/execution/events/order_event.h`

Represents an order state change.

```cpp
enum class OrderEventStatus
{
  NEW,
  SUBMITTED,
  ACCEPTED,
  PARTIALLY_FILLED,
  FILLED,
  CANCELED,
  EXPIRED,
  REJECTED,
  REPLACED
};

struct OrderEvent
{
  using Listener = IOrderExecutionListener;

  OrderEventStatus status = OrderEventStatus::NEW;
  Order order{};
  Order newOrder{};              // For REPLACED status
  Quantity fillQty{0};           // For PARTIALLY_FILLED

  uint64_t tickSequence{0};      // Internal bus sequence
  uint64_t recvNs{0};
  uint64_t publishNs{0};
  int64_t exchangeTsNs{0};

  void dispatchTo(IOrderExecutionListener& listener) const;
};
```

---

## OrderExecutionBus

**Header:** `flox/execution/bus/order_execution_bus.h`

```cpp
using OrderExecutionBus = EventBus<OrderEvent>;
```

Delivers order state changes to listeners.

---

## IOrderExecutor

**Header:** `flox/execution/abstract_executor.h`

Interface for submitting orders.

```cpp
class IOrderExecutor : public ISubsystem
{
public:
  virtual ~IOrderExecutor() = default;

  virtual void submitOrder(const Order& order) {}
  virtual void cancelOrder(OrderId orderId) {}
  virtual void replaceOrder(OrderId oldOrderId, const Order& newOrder) {}
};
```

### Implementation Notes

- All methods have empty default implementations
- Override only what you need
- Typically wraps exchange-specific order submission

---

## IOrderExecutionListener

**Header:** `flox/execution/abstract_execution_listener.h`

Receives order state changes.

```cpp
class IOrderExecutionListener : public ISubscriber
{
public:
  virtual ~IOrderExecutionListener() = default;

  virtual void onOrderSubmitted(const Order& order) {}
  virtual void onOrderAccepted(const Order& order) {}
  virtual void onOrderPartiallyFilled(const Order& order, Quantity filledQty) {}
  virtual void onOrderFilled(const Order& order) {}
  virtual void onOrderCanceled(const Order& order) {}
  virtual void onOrderExpired(const Order& order) {}
  virtual void onOrderRejected(const Order& order, const std::string& reason) {}
  virtual void onOrderReplaced(const Order& oldOrder, const Order& newOrder) {}
};
```

---

## IRiskManager

**Header:** `flox/risk/abstract_risk_manager.h`

Pre-trade risk checks.

```cpp
class IRiskManager : public ISubsystem
{
public:
  virtual ~IRiskManager() = default;
  virtual bool allow(const Order& order) const = 0;
};
```

### Usage

```cpp
if (!_riskManager->allow(order)) {
  // Order blocked by risk
  return;
}
_executor->submitOrder(order);
```

---

## IKillSwitch

**Header:** `flox/killswitch/abstract_killswitch.h`

Emergency stop mechanism.

```cpp
class IKillSwitch : public ISubsystem
{
public:
  virtual ~IKillSwitch() = default;

  virtual void check(const Order& order) = 0;
  virtual void trigger(const std::string& reason) = 0;
  virtual bool isTriggered() const = 0;
  virtual std::string reason() const = 0;
};
```

### Usage

```cpp
_killSwitch->check(order);

if (_killSwitch->isTriggered()) {
  FLOX_LOG("Kill switch triggered: " << _killSwitch->reason());
  return;
}

_executor->submitOrder(order);
```

---

## IOrderValidator

**Header:** `flox/validation/abstract_order_validator.h`

Order validation before submission.

```cpp
class IOrderValidator : public ISubsystem
{
public:
  virtual ~IOrderValidator() = default;

  virtual bool validate(const Order& order, std::string& reason) const = 0;
};
```

### Usage

```cpp
std::string reason;
if (!_validator->validate(order, reason)) {
  FLOX_LOG("Order validation failed: " << reason);
  return;
}
```

---

## ExecutionTrackerAdapter

**Header:** `flox/execution/execution_tracker_adapter.h`

Adapts `OrderExecutionBus` events to `IExecutionTracker`.

```cpp
class ExecutionTrackerAdapter : public IOrderExecutionListener
{
public:
  ExecutionTrackerAdapter(SymbolId symbol, IExecutionTracker* tracker);
};
```

### Usage

```cpp
auto tracker = std::make_unique<MyExecutionTracker>();
auto adapter = std::make_unique<ExecutionTrackerAdapter>(symbolId, tracker.get());

execBus->subscribe(adapter.get());
```

---

## Complete Execution Flow

```cpp
class MyStrategy : public IStrategy
{
  IOrderExecutor* _executor;
  IRiskManager* _riskManager;
  IKillSwitch* _killSwitch;
  IOrderValidator* _validator;

  void onTrade(const TradeEvent& ev) override {
    if (!shouldTrade(ev)) return;

    Order order = buildOrder(ev);

    // 1. Kill switch check
    _killSwitch->check(order);
    if (_killSwitch->isTriggered()) {
      return;
    }

    // 2. Validation
    std::string reason;
    if (!_validator->validate(order, reason)) {
      return;
    }

    // 3. Risk check
    if (!_riskManager->allow(order)) {
      return;
    }

    // 4. Submit
    _executor->submitOrder(order);
  }
};
```

---

## See Also

- [Market Data Reference](market-data.md) — Events and buses
- [First Strategy Tutorial](../tutorials/first-strategy.md) — Strategy example
