# Position tracking

---

## PositionTracker

FIFO/average-cost position tracking.

| Constant | Value | Description |
|----------|-------|-------------|
| `flox.POSITION_FIFO` | `0` | First-in first-out (default) |
| `flox.POSITION_AVG_COST` | `1` | Average cost basis |

```javascript
const tracker = new flox.PositionTracker(flox.POSITION_FIFO);
const tracker = new flox.PositionTracker(flox.POSITION_AVG_COST);
```

| Method | Returns | Description |
|--------|---------|-------------|
| `onFill(symbol, side, price, qty)` | `void` | Record a fill |
| `position(symbol)` | `number` | Current net position |
| `avgEntryPrice(symbol)` | `number` | Average entry price |
| `realizedPnl(symbol)` | `number` | Realized PnL for symbol |
| `totalRealizedPnl()` | `number` | Total realized PnL |

---

## PositionGroupTracker

Tracks individual named positions (open/partial-close/close).

```javascript
const tracker = new flox.PositionGroupTracker();
const posId = tracker.openPosition(orderId, symbol, 'buy', price, qty);
tracker.closePosition(posId, exitPrice);
```

| Method | Returns | Description |
|--------|---------|-------------|
| `openPosition(orderId, symbol, side, price, qty)` | `number` (position ID) | Open a position |
| `closePosition(positionId, exitPrice)` | `void` | Close a position fully |
| `partialClose(positionId, qty, exitPrice)` | `void` | Partial close |
| `netPosition(symbol)` | `number` | Net position for symbol |
| `realizedPnl(symbol)` | `number` | Realized PnL for symbol |
| `totalRealizedPnl()` | `number` | Total realized PnL |
| `openCount(symbol)` | `number` | Number of open positions |
| `prune()` | `void` | Free closed position memory |

---

## OrderTracker

Tracks submitted/filled/canceled orders.

```javascript
const tracker = new flox.OrderTracker();
tracker.onSubmitted(orderId, symbol, 'buy', price, qty);
tracker.onFilled(orderId, fillQty);
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `onSubmitted(orderId, symbol, side, price, qty)` | `boolean` | Register a submitted order |
| `onFilled(orderId, fillQty)` | `boolean` | Record a fill |
| `onCanceled(orderId)` | `boolean` | Record a cancellation |
| `isActive(orderId)` | `boolean` | True if order is still active |
| `activeCount` | `number` | Number of active orders (property) |
| `totalCount` | `number` | Total orders seen (property) |
| `prune()` | `void` | Free completed order memory |
