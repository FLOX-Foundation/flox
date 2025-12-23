# Advanced Orders

This guide covers the advanced order types available in FLOX, including conditional orders, OCO, and execution flags.

## Order Types Overview

| Type | Use Case |
|------|----------|
| `STOP_MARKET` | Stop loss, breakout entry |
| `STOP_LIMIT` | Stop with price control |
| `TAKE_PROFIT_MARKET` | Lock in profits |
| `TAKE_PROFIT_LIMIT` | Lock in profits with price control |
| `TRAILING_STOP` | Dynamic stop that follows price |
| OCO | One-Cancels-Other for breakouts |

## Using Signal Factory Methods

### Stop Orders

```cpp
// Stop market - triggers market order when price crosses trigger
emitStopMarket(symbol, Side::SELL, Price::fromDouble(95.0), qty);

// Stop limit - triggers limit order when price crosses trigger
emitStopLimit(symbol, Side::SELL,
              Price::fromDouble(95.0),   // trigger
              Price::fromDouble(94.5),   // limit price
              qty);
```

**Stop trigger logic:**

- SELL stop: triggers when price <= triggerPrice (falling)
- BUY stop: triggers when price >= triggerPrice (rising)

### Take Profit Orders

```cpp
// Take profit market - exits position at profit target
emitTakeProfitMarket(symbol, Side::SELL, Price::fromDouble(110.0), qty);

// Take profit limit
emitTakeProfitLimit(symbol, Side::SELL,
                    Price::fromDouble(110.0),  // trigger
                    Price::fromDouble(109.5),  // limit price
                    qty);
```

**Take profit trigger logic:**

- SELL TP: triggers when price >= triggerPrice (rising, lock profit on long)
- BUY TP: triggers when price <= triggerPrice (falling, lock profit on short)

### Trailing Stop

```cpp
// Fixed offset trailing stop (follows price by 5.0)
emitTrailingStop(symbol, Side::SELL, Price::fromDouble(5.0), qty);

// Percentage trailing stop (follows price by 2%)
emitTrailingStopPercent(symbol, Side::SELL, 200, qty);  // 200 bps = 2%
```

**Trailing stop behavior:**

- SELL trailing: trigger follows price UP (never down)
- When price drops to trigger -> order executes

## Time-In-Force Options

```cpp
// IOC (Immediate-Or-Cancel) - fill immediately or cancel
emitLimitBuy(symbol, price, qty, TimeInForce::IOC);

// FOK (Fill-Or-Kill) - fill completely or reject
emitLimitBuy(symbol, price, qty, TimeInForce::FOK);

// POST_ONLY - maker only, reject if would take
emitLimitBuy(symbol, price, qty, TimeInForce::POST_ONLY);
```

## Execution Flags

### Using Signal Modifiers

```cpp
auto signal = Signal::limitBuy(symbol, price, qty, orderId)
                  .withTimeInForce(TimeInForce::IOC)
                  .withReduceOnly()
                  .withPostOnly();
emit(signal);
```

### Close Position

```cpp
// Close entire position with reduce-only market order
emitClosePosition(symbol);
```

## Checking Exchange Capabilities

Before using advanced features, check if they're supported:

```cpp
void MyStrategy::onStart() {
  auto caps = engine().executor().capabilities();

  _useTrailingStop = caps.supports(OrderType::TRAILING_STOP);
  _useOCO = caps.supportsOCO;

  if (!_useTrailingStop) {
    log().warn("Trailing stop not supported, using manual logic");
  }
}
```

## Example: Manual TP/SL Management

Since strategies have full control over order lifecycle, you can implement custom TP/SL logic:

```cpp
void onOrderFilled(const Order& entryOrder) {
  if (entryOrder.id == _entryOrderId) {
    Price entryPrice = entryOrder.price;

    // Place TP
    _tpOrderId = emitTakeProfitMarket(
        entryOrder.symbol, Side::SELL,
        Price::fromDouble(entryPrice.toDouble() * 1.06), // 6% profit
        entryOrder.quantity);

    // Place SL
    _slOrderId = emitStopMarket(
        entryOrder.symbol, Side::SELL,
        Price::fromDouble(entryPrice.toDouble() * 0.98), // 2% stop
        entryOrder.quantity);
  }

  // When TP fills, cancel SL
  if (entryOrder.id == _tpOrderId) {
    emitCancel(_slOrderId);
  }

  // When SL fills, cancel TP
  if (entryOrder.id == _slOrderId) {
    emitCancel(_tpOrderId);
  }
}
```

## Example: Trailing Stop for Profit Protection

```cpp
void onOrderFilled(const Order& order) {
  if (order.side == Side::BUY) {
    // Place trailing stop to protect profits
    emitTrailingStopPercent(order.symbol, Side::SELL, 300, order.quantity);
  }
}
```

## See Also

* [Order Types](../reference/api/common.md#ordertype)
* [Time-In-Force](../reference/api/common.md#timeinforce)
* [Order Structure](../reference/api/execution/order.md)
* [Exchange Capabilities](../reference/api/execution/exchange_capabilities.md)
