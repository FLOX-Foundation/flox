# Backtest Components

Standalone simulated exchange for step-by-step backtesting and fill analysis.

## SimulatedExecutor

A simulated exchange that matches orders against bar closes and trade prices. Supports market, limit, stop, take-profit, and trailing stop orders.

```python
executor = flox.SimulatedExecutor()
```

### Methods

#### `submit_order(id, side, price, quantity, type="market", symbol=1)`

Submit an order to the simulated exchange.

```python
executor.submit_order(id=1, side="buy", price=0.0, quantity=1.0,
                       type="market", symbol=1)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | `int` | — | Order ID |
| `side` | `str` | — | `"buy"` or `"sell"` |
| `price` | `float` | — | Limit/trigger price (0 for market) |
| `quantity` | `float` | — | Order size |
| `type` | `str` | `"market"` | Order type (see below) |
| `symbol` | `int` | `1` | Symbol ID |

**Order types:** `"market"`, `"limit"`, `"stop_market"`, `"stop_limit"`, `"take_profit_market"`, `"take_profit_limit"`, `"trailing_stop"`.

#### `cancel_order(order_id)`

Cancel an order by ID.

#### `cancel_all(symbol)`

Cancel all orders for a symbol.

#### `on_bar(symbol, close_price)`

Feed a bar close price for order matching. Limit orders are matched against this price.

```python
executor.on_bar(symbol=1, close_price=50000.0)
```

#### `on_trade(symbol, price, is_buy)`

Feed a trade for order matching.

```python
executor.on_trade(symbol=1, price=50000.0, is_buy=True)
```

#### `advance_clock(timestamp_ns)`

Advance the simulation clock.

```python
executor.advance_clock(timestamp_ns=1704067200_000_000_000)
```

#### `fills() -> ndarray`

Get all fills as a numpy structured array with `PyFill` dtype.

```python
fills = executor.fills()
for i in range(len(fills)):
    print(f"Fill: order={fills[i]['order_id']}, price={fills[i]['price_raw']/1e8}")
```

#### `fills_list() -> list[dict]`

Get fills as a list of dicts (more convenient, less performant).

```python
for fill in executor.fills_list():
    print(f"{fill['side']} {fill['quantity']} @ {fill['price']}")
```

| Key | Type | Description |
|-----|------|-------------|
| `order_id` | `int` | Order ID |
| `symbol` | `int` | Symbol ID |
| `side` | `str` | `"buy"` or `"sell"` |
| `price` | `float` | Fill price |
| `quantity` | `float` | Fill quantity |
| `timestamp_ns` | `int` | Fill timestamp |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `fill_count` | `int` | Number of fills |

---

## Structured Dtypes

### PyFill

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `uint64` | Order ID |
| `symbol` | `uint32` | Symbol ID |
| `side` | `uint8` | 0 = buy, 1 = sell |
| `price_raw` | `int64` | Fill price * 10^8 |
| `quantity_raw` | `int64` | Fill quantity * 10^8 |
| `timestamp_ns` | `int64` | Fill timestamp (ns) |

### PyTradeRecord

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `uint32` | Symbol ID |
| `side` | `uint8` | 0 = buy, 1 = sell |
| `entry_price_raw` | `int64` | Entry price * 10^8 |
| `exit_price_raw` | `int64` | Exit price * 10^8 |
| `quantity_raw` | `int64` | Trade quantity * 10^8 |
| `entry_time_ns` | `int64` | Entry timestamp |
| `exit_time_ns` | `int64` | Exit timestamp |
| `pnl_raw` | `int64` | PnL * 10^8 |
| `fee_raw` | `int64` | Fee * 10^8 |

---

## Example

```python
import flox_py as flox

executor = flox.SimulatedExecutor()

# Simulate a simple trade
executor.advance_clock(1_000_000_000)  # t=1s
executor.submit_order(id=1, side="buy", price=0, quantity=1.0)
executor.on_bar(symbol=1, close_price=50000.0)

executor.advance_clock(2_000_000_000)  # t=2s
executor.submit_order(id=2, side="sell", price=0, quantity=1.0)
executor.on_bar(symbol=1, close_price=51000.0)

for fill in executor.fills_list():
    print(f"{fill['side']} {fill['quantity']} @ {fill['price']}")
```
