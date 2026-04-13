# Order Books

Order book implementations for managing price levels and simulating market impact.

## OrderBook

N-level order book with snapshot and delta support. Backed by the C++ `NLevelOrderBook<8192>`.

```python
book = flox.OrderBook(tick_size=0.01)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `tick_size` | `float` | Minimum price increment |

### Methods

#### `apply_snapshot(bid_prices, bid_quantities, ask_prices, ask_quantities)`

Apply a full book snapshot (replaces existing book state).

```python
book.apply_snapshot(
    bid_prices=np.array([100.0, 99.99, 99.98]),
    bid_quantities=np.array([10.0, 25.0, 15.0]),
    ask_prices=np.array([100.01, 100.02, 100.03]),
    ask_quantities=np.array([8.0, 20.0, 12.0]),
)
```

#### `apply_delta(bid_prices, bid_quantities, ask_prices, ask_quantities)`

Apply an incremental update (quantity=0 removes level).

```python
book.apply_delta(
    bid_prices=np.array([100.0]),
    bid_quantities=np.array([15.0]),  # update qty
    ask_prices=np.array([100.01]),
    ask_quantities=np.array([0.0]),   # remove level
)
```

#### `best_bid() -> float | None`

Best bid price, or `None` if book is empty.

#### `best_ask() -> float | None`

Best ask price, or `None` if book is empty.

#### `mid() -> float | None`

Mid price `(best_bid + best_ask) / 2`, or `None`.

#### `spread() -> float | None`

Bid-ask spread, or `None`.

#### `bid_at_price(price) -> float`

Quantity at a specific bid price level.

#### `ask_at_price(price) -> float`

Quantity at a specific ask price level.

#### `get_bids(max_levels=20) -> ndarray`

Get bid levels as Nx2 array `[price, quantity]`, sorted best-to-worst.

```python
bids = book.get_bids(max_levels=10)
# bids[0] = [best_bid_price, best_bid_qty]
```

#### `get_asks(max_levels=20) -> ndarray`

Get ask levels as Nx2 array `[price, quantity]`, sorted best-to-worst.

#### `consume_asks(quantity) -> tuple[float, float]`

Simulate a market buy order. Returns `(filled_qty, total_cost)`.

```python
filled, cost = book.consume_asks(100.0)
avg_price = cost / filled if filled > 0 else 0
```

#### `consume_bids(quantity) -> tuple[float, float]`

Simulate a market sell order. Returns `(filled_qty, total_cost)`.

#### `is_crossed() -> bool`

Check if the book is in a crossed state (best bid >= best ask).

#### `clear()`

Remove all price levels.

---

## L3Book

Level-3 order book with individual order tracking. Backed by `L3OrderBook<8192>`.

```python
book = flox.L3Book()
```

### Methods

#### `add_order(order_id, price, quantity, side) -> str`

Add an order. Returns status: `"ok"`, `"no_capacity"`, `"extant"`.

```python
status = book.add_order(order_id=1, price=100.0, quantity=10.0, side="buy")
```

#### `remove_order(order_id) -> str`

Remove an order by ID. Returns `"ok"` or `"not_found"`.

#### `modify_order(order_id, new_quantity) -> str`

Modify an order's quantity. Returns `"ok"` or `"not_found"`.

#### `best_bid() -> float | None`

Best bid price, or `None`.

#### `best_ask() -> float | None`

Best ask price, or `None`.

#### `bid_at_price(price) -> float`

Total bid quantity at a price level.

#### `ask_at_price(price) -> float`

Total ask quantity at a price level.

#### `export_snapshot() -> list[dict]`

Export all orders as a list of dicts.

```python
orders = book.export_snapshot()
# [{"id": 1, "price": 100.0, "quantity": 10.0, "side": "buy"}, ...]
```

#### `build_from_snapshot(orders)`

Rebuild the book from a snapshot.

```python
book.build_from_snapshot([
    {"id": 1, "price": 100.0, "quantity": 10.0, "side": "buy"},
    {"id": 2, "price": 100.01, "quantity": 5.0, "side": "sell"},
])
```

---

## CompositeBookMatrix

Cross-exchange composite order book. Tracks best bid/ask across up to 4 exchanges per symbol with lock-free atomic reads.

```python
matrix = flox.CompositeBookMatrix(staleness_threshold_ms=5000)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `staleness_threshold_ms` | `int` | `5000` | Time in ms before a quote is considered stale |

### Methods

#### `update_book(exchange, symbol, bid_prices, bid_quantities, ask_prices, ask_quantities, recv_ns=0)`

Feed a book update from an exchange.

```python
matrix.update_book(
    exchange=0, symbol=1,
    bid_prices=np.array([50000.0]),
    bid_quantities=np.array([1.5]),
    ask_prices=np.array([50001.0]),
    ask_quantities=np.array([2.0]),
    recv_ns=1704067200_000_000_000,
)
```

#### `best_bid(symbol) -> dict | None`

Best bid across all non-stale exchanges. Returns `{"price": float, "quantity": float, "exchange": int}` or `None`.

```python
bid = matrix.best_bid(symbol=1)
if bid:
    print(f"Best bid: {bid['price']} on exchange {bid['exchange']}")
```

#### `best_ask(symbol) -> dict | None`

Best ask across all non-stale exchanges.

#### `bid_for_exchange(symbol, exchange) -> dict | None`

Best bid on a specific exchange.

#### `ask_for_exchange(symbol, exchange) -> dict | None`

Best ask on a specific exchange.

#### `has_arbitrage_opportunity(symbol) -> bool`

Returns `True` if the best bid on one exchange exceeds the best ask on another.

```python
if matrix.has_arbitrage_opportunity(symbol=1):
    bid = matrix.best_bid(1)
    ask = matrix.best_ask(1)
    profit = bid['price'] - ask['price']
    print(f"Arb: buy on ex{ask['exchange']} sell on ex{bid['exchange']}, profit={profit}")
```

#### `spread(symbol) -> float | None`

Composite spread (best ask - best bid), or `None` if no valid quotes.

#### `mark_stale(exchange, symbol)`

Mark a specific exchange+symbol pair as stale (excluded from best quote queries).

#### `mark_exchange_stale(exchange)`

Mark all symbols on an exchange as stale (e.g., on disconnect).

#### `check_staleness(now_ns)`

Check all entries against the staleness threshold configured at construction.

```python
import time
matrix.check_staleness(int(time.time_ns()))
```
