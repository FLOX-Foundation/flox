# Positions

Position tracking, group management, and order lifecycle tracking.

## PositionTracker

Tracks net positions per symbol with configurable cost basis methods.

```python
tracker = flox.PositionTracker(cost_basis="fifo")
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cost_basis` | `str` | `"fifo"` | Cost basis method: `"fifo"`, `"lifo"`, or `"average"` |

### Methods

#### `on_fill(symbol, side, price, quantity)`

Record a fill.

```python
tracker.on_fill(symbol=1, side="buy", price=50000.0, quantity=1.0)
tracker.on_fill(symbol=1, side="sell", price=51000.0, quantity=0.5)
```

#### `position(symbol) -> float`

Signed net position for a symbol. Positive = long, negative = short.

#### `avg_entry_price(symbol) -> float`

Average entry price for current position.

#### `realized_pnl(symbol) -> float`

Realized PnL for a symbol.

#### `total_realized_pnl() -> float`

Total realized PnL across all symbols.

---

## PositionGroupTracker

Individual position management with grouping support. Track each trade as a separate position, organize into groups, and compute per-group metrics.

```python
tracker = flox.PositionGroupTracker()
```

### Methods

#### `open_position(order_id, symbol, side, price, quantity) -> int`

Open a new position. Returns a position ID.

```python
pid = tracker.open_position(order_id=1, symbol=1, side="buy",
                             price=50000.0, quantity=1.0)
```

#### `close_position(position_id, exit_price)`

Close a position at the given exit price.

```python
tracker.close_position(pid, exit_price=51000.0)
```

#### `partial_close(position_id, quantity, exit_price)`

Partially close a position.

```python
tracker.partial_close(pid, quantity=0.5, exit_price=50500.0)
```

#### `create_group(parent_id=0) -> int`

Create a position group. Returns a group ID.

```python
gid = tracker.create_group()
```

#### `assign_to_group(position_id, group_id) -> bool`

Assign a position to a group.

#### `get_position(position_id) -> dict | None`

Get position details.

| Key | Type | Description |
|-----|------|-------------|
| `position_id` | `int` | Position ID |
| `order_id` | `int` | Original order ID |
| `symbol` | `int` | Symbol ID |
| `side` | `str` | `"buy"` or `"sell"` |
| `entry_price` | `float` | Entry price |
| `quantity` | `float` | Remaining quantity |
| `realized_pnl` | `float` | Realized PnL |
| `closed` | `bool` | Whether fully closed |
| `group_id` | `int` | Assigned group (0 = none) |

#### `net_position(symbol) -> float`

Net position for a symbol.

#### `group_net_position(group_id) -> float`

Net position for a group.

#### `realized_pnl(symbol) -> float`

Realized PnL for a symbol.

#### `total_realized_pnl() -> float`

Total realized PnL.

#### `group_realized_pnl(group_id) -> float`

Realized PnL for a group.

#### `group_unrealized_pnl(group_id, current_price) -> float`

Unrealized PnL for a group at the given market price.

#### `open_positions(symbol) -> list[dict]`

List of open position dicts for a symbol.

#### `open_position_count(symbol=None) -> int`

Count of open positions. Pass `None` for total across all symbols.

#### `prune_closed()`

Remove closed positions from memory.

---

## OrderTracker

Order lifecycle tracking: submitted, filled, canceled, rejected.

```python
tracker = flox.OrderTracker()
```

### Methods

#### `on_submitted(order_id, exchange_order_id, client_order_id="") -> bool`

Record an order submission.

```python
tracker.on_submitted(order_id=1, exchange_order_id="EX-12345")
```

#### `on_filled(order_id, fill_quantity) -> bool`

Record a fill (partial or full).

#### `on_canceled(order_id) -> bool`

Record a cancellation.

#### `on_rejected(order_id, reason) -> bool`

Record a rejection.

#### `get(order_id) -> dict | None`

Get order state.

| Key | Type | Description |
|-----|------|-------------|
| `order_id` | `int` | Local order ID |
| `exchange_order_id` | `str` | Exchange-assigned ID |
| `client_order_id` | `str` | Client-assigned ID |
| `status` | `str` | Current status (see below) |
| `filled` | `float` | Filled quantity |
| `is_terminal` | `bool` | Whether order is in a terminal state |

**Status values:** `"new"`, `"submitted"`, `"accepted"`, `"partially_filled"`, `"filled"`, `"pending_cancel"`, `"canceled"`, `"expired"`, `"rejected"`, `"replaced"`, `"pending_trigger"`, `"triggered"`, `"trailing_updated"`.

#### `is_active(order_id) -> bool`

Check if an order is still active (non-terminal).

#### `active_count() -> int`

Number of active orders.

#### `total_count() -> int`

Total tracked orders.

#### `prune_terminal()`

Remove terminal orders from memory.
