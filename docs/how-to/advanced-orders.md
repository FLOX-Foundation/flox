# Advanced orders

Stop orders, take-profit, trailing stops, OCO, and execution flags. Every order type below is exposed through every binding — only the method name differs.

## Order types

| Type | Use case |
|------|----------|
| `STOP_MARKET` | Stop loss, breakout entry |
| `STOP_LIMIT` | Stop with price control |
| `TAKE_PROFIT_MARKET` | Lock in profits |
| `TAKE_PROFIT_LIMIT` | Lock in profits with price control |
| `TRAILING_STOP` | Dynamic stop that follows price |
| `OCO` | One-cancels-other for breakouts |

## Stop orders

Stop trigger logic: SELL stop fires when `price <= trigger`; BUY stop fires when `price >= trigger`.

=== "Python"

    ```python
    # Stop market: stop loss for a long
    self.stop_market(side="sell", trigger=95.0, qty=qty)

    # Stop limit
    self.stop_limit(side="sell", trigger=95.0, limit_price=94.5, qty=qty)
    ```

=== "Node.js"

    ```javascript
    emit.stopMarket("sell", 95.0, qty);
    emit.stopLimit("sell", 95.0, 94.5, qty);
    ```

=== "Codon"

    ```python
    self.stop_market("sell", 95.0, qty)
    self.stop_limit("sell", 95.0, 94.5, qty)
    ```

=== "C++"

    ```cpp
    emitStopMarket(symbol, Side::SELL, Price::fromDouble(95.0), qty);
    emitStopLimit (symbol, Side::SELL, Price::fromDouble(95.0),
                                       Price::fromDouble(94.5), qty);
    ```

## Take-profit orders

SELL TP fires when `price >= trigger` (lock profit on a long); BUY TP fires when `price <= trigger`.

=== "Python"

    ```python
    self.take_profit_market(side="sell", trigger=110.0, qty=qty)
    self.take_profit_limit (side="sell", trigger=110.0, limit_price=109.5, qty=qty)
    ```

=== "Node.js"

    ```javascript
    emit.takeProfitMarket("sell", 110.0, qty);
    emit.takeProfitLimit ("sell", 110.0, 109.5, qty);
    ```

=== "Codon"

    ```python
    self.take_profit_market("sell", 110.0, qty)
    self.take_profit_limit ("sell", 110.0, 109.5, qty)
    ```

=== "C++"

    ```cpp
    emitTakeProfitMarket(symbol, Side::SELL, Price::fromDouble(110.0), qty);
    emitTakeProfitLimit (symbol, Side::SELL,
                          Price::fromDouble(110.0), Price::fromDouble(109.5), qty);
    ```

## Trailing stops

SELL trailing trigger follows price up; when price drops to trigger, the order executes.

=== "Python"

    ```python
    # Fixed-offset trailing stop (5.0 below price)
    self.trailing_stop(side="sell", offset=5.0, qty=qty)

    # Percentage trailing stop (2% = 200 bps)
    self.trailing_stop_percent(side="sell", callback_bps=200, qty=qty)
    ```

=== "Node.js"

    ```javascript
    emit.trailingStop("sell", 5.0, qty);
    emit.trailingStopPercent("sell", 200, qty);   // 200 bps = 2%
    ```

=== "Codon"

    ```python
    self.trailing_stop("sell", 5.0, qty)
    self.trailing_stop_percent("sell", 200, qty)
    ```

=== "C++"

    ```cpp
    emitTrailingStop       (symbol, Side::SELL, Price::fromDouble(5.0), qty);
    emitTrailingStopPercent(symbol, Side::SELL, /*callback_bps=*/200,   qty);
    ```

## Time-in-force

`IOC` (immediate-or-cancel), `FOK` (fill-or-kill), `POST_ONLY` (maker-only — reject if would cross spread).

=== "Python"

    ```python
    self.limit_buy(price, qty, tif="ioc")
    self.limit_buy(price, qty, tif="fok")
    self.limit_buy(price, qty, tif="post_only")
    ```

=== "Node.js"

    ```javascript
    emit.limitBuy(price, qty, /*tif=*/ "IOC");
    emit.limitBuy(price, qty, /*tif=*/ "FOK");
    emit.limitBuy(price, qty, /*tif=*/ "POST_ONLY");
    ```

=== "Codon"

    ```python
    self.limit_buy(price, qty, tif="ioc")
    self.limit_buy(price, qty, tif="fok")
    self.limit_buy(price, qty, tif="post_only")
    ```

=== "C++"

    ```cpp
    emitLimitBuy(symbol, price, qty, TimeInForce::IOC);
    emitLimitBuy(symbol, price, qty, TimeInForce::FOK);
    emitLimitBuy(symbol, price, qty, TimeInForce::POST_ONLY);
    ```

`POST_ONLY` orders that would cross the book are rejected at submit time (matches real-exchange behaviour, including in `BacktestRunner` since [#124](https://github.com/FLOX-Foundation/flox/pull/124)).

## Close current position

`close_position` issues a reduce-only market order sized to flatten the existing position.

=== "Python"

    ```python
    self.close_position()         # primary symbol
    self.close_position("BTCUSDT")
    ```

=== "Node.js"

    ```javascript
    emit.closePosition();
    emit.closePosition("BTCUSDT");
    ```

=== "Codon"

    ```python
    self.close_position()
    ```

=== "C++"

    ```cpp
    emitClosePosition(symbol);
    ```

## Manual TP/SL bracket

A common pattern: on entry-fill, place TP and SL; cancel one when the other fills.

=== "Python"

    ```python
    def on_order_filled(self, order):
        if order.id == self.entry_id:
            entry = order.price
            self.tp_id = self.take_profit_market("sell", trigger=entry * 1.06, qty=order.quantity)
            self.sl_id = self.stop_market       ("sell", trigger=entry * 0.98, qty=order.quantity)
        elif order.id == self.tp_id:
            self.cancel_order(self.sl_id)
        elif order.id == self.sl_id:
            self.cancel_order(self.tp_id)
    ```

=== "Node.js"

    ```javascript
    onOrderFilled(order, emit) {
      if (order.id === this.entryId) {
        const entry = order.price;
        this.tpId = emit.takeProfitMarket("sell", entry * 1.06, order.quantity);
        this.slId = emit.stopMarket       ("sell", entry * 0.98, order.quantity);
      } else if (order.id === this.tpId) {
        emit.cancel(this.slId);
      } else if (order.id === this.slId) {
        emit.cancel(this.tpId);
      }
    }
    ```

=== "C++"

    ```cpp
    void onOrderFilled(const Order& order) {
      if (order.id == _entryId) {
        auto entry = order.price.toDouble();
        _tpId = emitTakeProfitMarket(order.symbol, Side::SELL,
                                      Price::fromDouble(entry * 1.06), order.quantity);
        _slId = emitStopMarket      (order.symbol, Side::SELL,
                                      Price::fromDouble(entry * 0.98), order.quantity);
      } else if (order.id == _tpId) {
        emitCancel(_slId);
      } else if (order.id == _slId) {
        emitCancel(_tpId);
      }
    }
    ```

## Checking exchange capabilities

Real exchanges support different subsets of order types. Before using an advanced feature in live trading, ask the executor what it supports.

=== "C++"

    ```cpp
    auto caps = engine().executor().capabilities();
    bool hasTrailing = caps.supports(OrderType::TRAILING_STOP);
    bool hasOCO      = caps.supportsOCO;
    ```

=== "Python / Node.js / Codon"

    Capabilities introspection isn't yet exposed in the binding APIs — check the [exchange's connector source](../bindings/README.md) or fall back to manual TP/SL.

## See also

- [Order Types reference](../reference/api/common.md)
- [Time-In-Force](../reference/api/common.md)
- [Order structure](../reference/api/execution/order.md)
- [Exchange capabilities](../reference/api/execution/exchange_capabilities.md)
