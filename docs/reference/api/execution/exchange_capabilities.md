# ExchangeCapabilities

`ExchangeCapabilities` provides feature discovery for execution venues, allowing strategies to check which order types and features are supported.

```cpp
struct ExchangeCapabilities
{
  // Order types
  bool supportsStopMarket{true};
  bool supportsStopLimit{true};
  bool supportsTakeProfitMarket{true};
  bool supportsTakeProfitLimit{true};
  bool supportsTrailingStop{false};
  bool supportsIceberg{false};

  // OCO
  bool supportsOCO{false};

  // Time-in-force
  bool supportsGTC{true};
  bool supportsIOC{true};
  bool supportsFOK{true};
  bool supportsGTD{false};
  bool supportsPostOnly{true};

  // Execution flags
  bool supportsReduceOnly{true};
  bool supportsClosePosition{false};

  bool supports(OrderType type) const;
  bool supports(TimeInForce tif) const;

  static ExchangeCapabilities all();
  static ExchangeCapabilities simulated();
};
```

Each capability is an independent `bool`. There is no bitmask. No member function is `noexcept`.

## Purpose

* Enable runtime feature discovery for exchange/executor capabilities.
* Allow strategies to adapt behavior based on available features.
* Prevent submission of unsupported order types.

## Fields

| Field | Default | Description |
|-------|---------|-------------|
| `supportsStopMarket` | `true` | `OrderType::STOP_MARKET` |
| `supportsStopLimit` | `true` | `OrderType::STOP_LIMIT` |
| `supportsTakeProfitMarket` | `true` | `OrderType::TAKE_PROFIT_MARKET` |
| `supportsTakeProfitLimit` | `true` | `OrderType::TAKE_PROFIT_LIMIT` |
| `supportsTrailingStop` | `false` | `OrderType::TRAILING_STOP` |
| `supportsIceberg` | `false` | `OrderType::ICEBERG` |
| `supportsOCO` | `false` | OCO (one-cancels-other) orders |
| `supportsGTC` | `true` | `TimeInForce::GTC` |
| `supportsIOC` | `true` | `TimeInForce::IOC` |
| `supportsFOK` | `true` | `TimeInForce::FOK` |
| `supportsGTD` | `false` | `TimeInForce::GTD` |
| `supportsPostOnly` | `true` | `TimeInForce::POST_ONLY` |
| `supportsReduceOnly` | `true` | `ExecutionFlags::reduceOnly` |
| `supportsClosePosition` | `false` | `ExecutionFlags::closePosition` |

There is no field for `OrderType::LIMIT` or `OrderType::MARKET` — `supports()` returns `true` for
both unconditionally.

## Methods

### `supports(OrderType type)`

Switches on `type` and returns the matching field. `LIMIT` and `MARKET` always return `true`; an
out-of-range value returns `false`.

```cpp
bool canUseStop = caps.supports(OrderType::STOP_MARKET);
```

### `supports(TimeInForce tif)`

Switches on `tif` and returns the matching field. An out-of-range value returns `false`.

```cpp
bool canUseIOC = caps.supports(TimeInForce::IOC);
```

## Factory Methods

### `ExchangeCapabilities::all()`

Default-constructs, then sets `supportsTrailingStop`, `supportsIceberg`, `supportsOCO`,
`supportsGTD` and `supportsClosePosition` to `true`. Every capability is then enabled.

### `ExchangeCapabilities::simulated()`

Returns `all()`. `SimulatedExecutor` supports everything.

`IOrderExecutor::capabilities()` defaults to `ExchangeCapabilities::simulated()`; a live connector
overrides it with the venue's real set.

## Usage

```cpp
auto caps = executor->capabilities();

if (caps.supports(OrderType::TRAILING_STOP))
{
  emitTrailingStop(symbol, Side::SELL, offset, qty);
}
else
{
  // Fall back to manual trailing logic.
}

if (caps.supports(TimeInForce::POST_ONLY))
{
  emitLimitBuy(symbol, price, qty, TimeInForce::POST_ONLY);
}

if (caps.supportsOCO)
{
  // Use OCO orders.
}
```

A default-constructed `ExchangeCapabilities` reports `supportsOCO == false`,
`supportsTrailingStop == false`, `supportsIceberg == false`, `supportsGTD == false` and
`supportsClosePosition == false`. Do not assume the permissive case; check, or start from `all()`.

## Handling Unsupported Features

When a strategy attempts to use an unsupported feature, the executor should reject the order with a
clear reason:

```cpp
if (!capabilities().supports(order.type))
{
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
