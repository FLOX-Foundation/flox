# ExchangeCapabilities

`ExchangeCapabilities` provides feature discovery for execution venues, allowing strategies to check which order types and features are supported.

```cpp
struct ExchangeCapabilities {
  uint32_t supportedOrderTypes{0};
  uint32_t supportedTimeInForce{0};
  bool supportsReduceOnly{true};
  bool supportsClosePosition{true};
  bool supportsOCO{true};

  bool supports(OrderType type) const noexcept;
  bool supports(TimeInForce tif) const noexcept;

  static ExchangeCapabilities all() noexcept;
  static ExchangeCapabilities simulated() noexcept;
};
```

## Purpose

* Enable runtime feature discovery for exchange/executor capabilities.
* Allow strategies to adapt behavior based on available features.
* Prevent submission of unsupported order types.

## Usage

```cpp
// Get capabilities from executor
auto caps = executor->capabilities();

// Check order type support
if (caps.supports(OrderType::TRAILING_STOP)) {
  emitTrailingStop(symbol, Side::SELL, offset, qty);
} else {
  // Fallback to manual trailing logic
}

// Check time-in-force support
if (caps.supports(TimeInForce::POST_ONLY)) {
  emitLimitBuy(symbol, price, qty, TimeInForce::POST_ONLY);
}

// Check OCO support
if (caps.supportsOCO) {
  // Use OCO orders
}
```

## Fields

| Field               | Description                                      |
|---------------------|--------------------------------------------------|
| supportedOrderTypes | Bitmask of supported `OrderType` values.         |
| supportedTimeInForce| Bitmask of supported `TimeInForce` values.       |
| supportsReduceOnly  | Whether reduceOnly flag is supported.            |
| supportsClosePosition| Whether closePosition flag is supported.        |
| supportsOCO         | Whether OCO (one-cancels-other) orders work.     |

## Methods

### `supports(OrderType type)`

Returns `true` if the given order type is supported.

```cpp
bool canUseStop = caps.supports(OrderType::STOP_MARKET);
```

### `supports(TimeInForce tif)`

Returns `true` if the given time-in-force policy is supported.

```cpp
bool canUseIOC = caps.supports(TimeInForce::IOC);
```

## Factory Methods

### `ExchangeCapabilities::all()`

Returns capabilities with all features enabled. Use for testing.

### `ExchangeCapabilities::simulated()`

Returns capabilities for `SimulatedExecutor`:
* All order types supported
* All time-in-force policies supported
* All flags supported
* OCO supported

## Implementation Notes

Order types are stored as a bitmask for efficient checking:

```cpp
bool supports(OrderType type) const noexcept {
  return (supportedOrderTypes & (1u << static_cast<uint8_t>(type))) != 0;
}
```

## Handling Unsupported Features

When a strategy attempts to use an unsupported feature, the executor should reject the order with a clear error:

```cpp
if (!capabilities().supports(order.type)) {
  OrderEvent ev;
  ev.status = OrderEventStatus::REJECTED;
  ev.order = order;
  ev.rejectReason = "Order type not supported by exchange";
  callback(ev);
  return;
}
```

## See Also

* [IOrderExecutor](abstract_executor.md) — Executor interface
* [OrderType](../common.md#ordertype) — Order types
* [TimeInForce](../common.md#timeinforce) — Time-in-force policies
