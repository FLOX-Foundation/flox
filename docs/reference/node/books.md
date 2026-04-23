# Order books

---

## OrderBook

L2 order book.

```javascript
const book = new flox.OrderBook(tickSize);
book.applySnapshot(bidPrices, bidQtys, askPrices, askQtys);
```

| Method | Returns | Description |
|--------|---------|-------------|
| `applySnapshot(bp, bq, ap, aq)` | `void` | Full snapshot (Float64Arrays) |
| `applyDelta(bp, bq, ap, aq)` | `void` | Incremental update |
| `bestBid()` | `number \| null` | Best bid price |
| `bestAsk()` | `number \| null` | Best ask price |
| `mid()` | `number \| null` | Mid price |
| `spread()` | `number \| null` | Bid-ask spread |
| `getBids(n)` | `[price, qty][]` | Top N bid levels |
| `getAsks(n)` | `[price, qty][]` | Top N ask levels |
| `isCrossed()` | `boolean` | True if book is crossed |
| `clear()` | `void` | Clear all levels |

---

## L3Book

Order-level book with individual order tracking.

```javascript
const book = new flox.L3Book();
book.addOrder(orderId, price, qty, 'buy');
```

| Method | Returns | Description |
|--------|---------|-------------|
| `addOrder(orderId, price, qty, side)` | `number` | 0 on success |
| `removeOrder(orderId)` | `number` | 0 on success |
| `modifyOrder(orderId, newQty)` | `number` | 0 on success |
| `bestBid()` | `number \| null` | Best bid price |
| `bestAsk()` | `number \| null` | Best ask price |
| `bidAtPrice(price)` | `number` | Total bid quantity at price |
| `askAtPrice(price)` | `number` | Total ask quantity at price |

---

## CompositeBookMatrix

Aggregates books across multiple exchanges per symbol.

```javascript
const matrix = new flox.CompositeBookMatrix();
```

| Method | Returns | Description |
|--------|---------|-------------|
| `bestBid(symbol)` | `{ price, qty } \| null` | Best bid across exchanges |
| `bestAsk(symbol)` | `{ price, qty } \| null` | Best ask across exchanges |
| `hasArbitrage(symbol)` | `boolean` | True if arbitrage opportunity exists |
| `markStale(exchange, symbol)` | `void` | Mark exchange data as stale |
| `checkStaleness(nowNs, thresholdNs)` | `void` | Evict stale data |
