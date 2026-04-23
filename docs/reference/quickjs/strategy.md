# Strategy

Extend `Strategy` and call `flox.register` with an instance.

```javascript
class MyStrategy extends Strategy {
    constructor() {
        super({ exchange: 'Binance', symbols: ['BTCUSDT'] });
    }

    onTrade(ctx, trade) { ... }
    onStart() { ... }
    onStop() { ... }
}

flox.register(new MyStrategy());
```

### Constructor options

```javascript
// Single exchange
super({ exchange: 'Binance', symbols: ['BTCUSDT', 'ETHUSDT'] })

// Multi-exchange (qualified names)
super({ symbols: ['Binance:BTCUSDT', 'Bybit:ETHUSDT'] })
```

### Properties

| Property | Type | Description |
|---|---|---|
| `primarySymbol` | `string` | First symbol name |
| `symbols` | `string[]` | All registered symbol names |
| `hasPosition` | `boolean` | Whether a position is open (primary symbol) |

---

## Callbacks

### `onTrade(ctx, trade)`

| Field | Type | Description |
|---|---|---|
| `ctx.symbol` | `string` | Symbol name |
| `ctx.symbolId` | `number` | Numeric symbol ID |
| `ctx.position` | `number` | Current position size |
| `ctx.avgEntryPrice` | `number` | Average entry price |
| `ctx.book.bidPrice` | `number` | Best bid |
| `ctx.book.askPrice` | `number` | Best ask |
| `trade.symbol` | `string` | Symbol name |
| `trade.price` | `number` | Trade price |
| `trade.qty` | `number` | Trade quantity |
| `trade.side` | `string` | `"buy"` or `"sell"` |
| `trade.isBuy` | `boolean` | |
| `trade.timestampNs` | `number` | Nanosecond timestamp |

### `onStart()` / `onStop()`

Called when the strategy starts and stops.

---

## Order methods

All methods accept an options object. `symbol` defaults to the primary symbol when omitted.

### Market

```javascript
this.marketBuy({ qty: 1.0 })
this.marketSell({ symbol: 'ETHUSDT', qty: 2.0 })
```

### Limit

```javascript
this.limitBuy({ price: 50000, qty: 0.1 })
this.limitSell({ price: 51000, qty: 0.1, tif: 'IOC' })  // tif: GTC (default) | IOC | FOK
```

### Stop / take-profit

```javascript
this.stopMarket({ side: 'sell', trigger: 48000, qty: 0.1 })
this.stopLimit({ side: 'buy', trigger: 52000, price: 52100, qty: 0.1 })
this.takeProfitMarket({ side: 'sell', trigger: 55000, qty: 0.1 })
this.takeProfitLimit({ side: 'sell', trigger: 55000, price: 54900, qty: 0.1 })
```

### Trailing stop

```javascript
this.trailingStop({ side: 'sell', offset: 100, qty: 0.1 })
this.trailingStopPercent({ side: 'sell', callbackBps: 50, qty: 0.1 })
```

### Order management

```javascript
this.cancel(orderId)
this.cancelAll()
this.modify(orderId, { price: 50100, qty: 0.2 })
this.closePosition()
```

---

## Context queries

```javascript
this.position()             // primary symbol
this.position('ETHUSDT')    // specific symbol
this.bestBid()
this.bestAsk()
this.midPrice()
this.lastPrice()
this.orderStatus(orderId)   // -1 if not found
```
