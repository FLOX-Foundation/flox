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

`tickSize` must be positive; the constructor throws a `RangeError` otherwise.
The smallest representable tick is `1e-8`, since prices are fixed-point with a
scale of 1e8. The book covers 8192 consecutive ticks centred on the market and
re-anchors that window as a delta feed walks the price away from the last
snapshot. See
[The order book's tick window](../../explanation/order-book-tick-window.md).

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
matrix.applySnapshot(exchangeId, symbol, bidPrices, bidQtys, askPrices, askQtys, recvNs);
```

| Method | Returns | Description |
|--------|---------|-------------|
| `applySnapshot(exchange, symbol, bp, bq, ap, aq, recvNs?)` | `void` | Full snapshot for one exchange's side of the symbol (Float64Arrays) |
| `applyDelta(exchange, symbol, bp, bq, ap, aq, recvNs?)` | `void` | Incremental update for one exchange's side; an absent side (empty arrays) is left untouched, not cleared |
| `bestBid(symbol)` | `{ price, qty } \| null` | Best bid across exchanges |
| `bestAsk(symbol)` | `{ price, qty } \| null` | Best ask across exchanges |
| `hasArbitrage(symbol)` | `boolean` | True if arbitrage opportunity exists |
| `markStale(exchange, symbol)` | `void` | Mark exchange data as stale |
| `checkStaleness(nowNs, thresholdNs)` | `void` | Evict stale data |

`applySnapshot` replaces both sides of that exchange's top-of-book wholesale,
including clearing a side that arrives as an empty array. `applyDelta` only
touches the side(s) actually present in the call -- a side passed as an empty
array is left exactly as it was, not zeroed. `recvNs` defaults to `0` and
feeds `checkStaleness`'s staleness clock; pass the actual receive timestamp
if you use staleness eviction. Codon (`CompositeBook.apply_snapshot` /
`apply_delta`) has the same two methods. QuickJS does not yet -- see
[the C API reference](../api/capi/flox_capi.md) for the underlying
`flox_composite_book_apply_snapshot` / `_apply_delta` functions if you need
to drive one from there in the meantime.
