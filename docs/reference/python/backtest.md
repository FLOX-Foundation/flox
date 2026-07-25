# Backtest Components

Standalone simulated exchange for step-by-step backtesting and fill analysis.

## SimulatedExecutor

A simulated exchange that matches orders against bar closes and trade prices. Supports market, limit, stop, take-profit, and trailing stop orders.

```python
executor = flox.SimulatedExecutor()
```

### Methods

#### `submit_order(id, side, price, quantity, type="market", symbol=1, tif="gtc", reduce_only=False, expires_at_ns=0, account_id=0, trigger=0.0, trailing_offset=0.0, trailing_bps=0)`

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
| `tif` | `str` | `"gtc"` | Time in force: `"gtc"`, `"ioc"`, `"fok"`, `"gtd"`, `"post_only"` |
| `reduce_only` | `bool` | `False` | Only reduce an existing position |
| `expires_at_ns` | `int` | `0` | GTD deadline |
| `account_id` | `int` | `0` | Self-trade-prevention account identifier |
| `trigger` | `float` | `0.0` | Trigger price for the conditional order types; falls back to `price` when unset |
| `trailing_offset` | `float` | `0.0` | Fixed-price offset for `"trailing_stop"` |
| `trailing_bps` | `int` | `0` | Bps offset for `"trailing_stop"` — set exactly one of the two |

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

#### `on_trade_qty(symbol, price, qty, is_buy)`

Feed a trade with quantity. Required for `QUEUE_FULL` queue simulation.

```python
executor.on_trade_qty(symbol=1, price=50000.0, qty=0.5, is_buy=True)
```

#### `on_best_levels(symbol, bid_price, bid_qty, ask_price, ask_qty)`

Feed a top-of-book snapshot.

```python
executor.on_best_levels(1, 49999.0, 2.0, 50001.0, 1.5)
```

#### `set_default_slippage(model, ticks=0, tick_size=0.0, bps=0.0, impact_coeff=0.0)`

Configure slippage for all symbols. `model` is a string.

```python
executor.set_default_slippage("fixed_bps", bps=2.0)
```

| `model` | Description |
|---------|-------------|
| `"none"` | No slippage |
| `"fixed_ticks"` | Fixed tick count — uses `ticks` and `tick_size` |
| `"fixed_bps"` | Fixed basis points — uses `bps` |
| `"volume_impact"` | Volume-proportional impact — uses `impact_coeff` |

`tick_size` is in price units; `0.0` falls back to one raw price unit.

#### `set_symbol_slippage(symbol, model, ticks=0, tick_size=0.0, bps=0.0, impact_coeff=0.0)`

Per-symbol slippage override. Same parameters as `set_default_slippage`.

#### `set_queue_model(model, depth=1)`

Configure limit order queue simulation. `model` is a string.

```python
executor.set_queue_model("tob", depth=1)
```

| `model` | Description |
|---------|-------------|
| `"none"` | Fill limit orders immediately at price |
| `"tob"` | Fill only when price trades through level |
| `"full"` | Model queue position; fill as volume passes |
| `"pro_rata"` | Split each trade across the level proportionally to size |
| `"pro_rata_with_fifo"` | First N orders at a level fill FIFO (`set_queue_fifo_top_n`), the rest pro-rata |
| `"top_pro_lmm"` | Reserve a share for the queue front (`set_top_priority_share`), then pro-rata with an LMM bonus |
| `"pro_rata_with_priority"` | Pro-rata weighted by `set_order_priority_multiplier` |

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

## BacktestResult

Computes statistics and equity curve from a `SimulatedExecutor`'s fills.

```python
result = flox.BacktestResult(initial_capital=10_000.0, fee_rate=0.0004)
result.ingest_executor(executor)
stats = result.stats()
print(stats['net_pnl'], stats['sharpe_ratio'])
```

### Constructor

```python
flox.BacktestResult(
    initial_capital=100000.0,
    fee_rate=0.0001,
    use_percentage_fee=True,
    fixed_fee_per_trade=0.0,
    risk_free_rate=0.0,
    annualization_factor=252.0,
)
```

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `record_fill(order_id, symbol, side, price, quantity, timestamp_ns)` | `None` | Record a single fill |
| `ingest_executor(executor)` | `None` | Drain all fills from a `SimulatedExecutor` in FIFO order |
| `stats()` | `dict` | Compute and return statistics |
| `trades()` | `ndarray` | Closed trades as a `PyTradeRecord` structured array |
| `equity_curve()` | `ndarray` | Equity curve as a `PyEquityPoint` structured array |
| `write_equity_curve_csv(path)` | `bool` | Write equity curve to CSV (`timestamp_ns,equity,drawdown_pct` header) |

### `stats()` keys

The [`Stats`](engine.md#stats) object returned by `Engine.run()` is a different, smaller set — note in particular `sharpe_ratio` / `sortino_ratio` / `calmar_ratio` here versus `sharpe` / `sortino` / `calmar` there.

| Key | Type | Description |
|-----|------|-------------|
| `total_trades` | `int` | Round-trip trade count |
| `winning_trades` | `int` | Profitable trade count |
| `losing_trades` | `int` | Losing trade count |
| `max_consecutive_wins` | `int` | Longest winning streak |
| `max_consecutive_losses` | `int` | Longest losing streak |
| `initial_capital` | `float` | Starting capital |
| `final_capital` | `float` | Ending capital |
| `total_pnl` | `float` | Gross PnL |
| `total_fees` | `float` | Total fees paid |
| `net_pnl` | `float` | PnL after fees |
| `gross_profit` | `float` | Sum of winning trades |
| `gross_loss` | `float` | Sum of losing trades |
| `max_drawdown` | `float` | Maximum drawdown (absolute) |
| `max_drawdown_pct` | `float` | Maximum drawdown (percentage) |
| `win_rate` | `float` | Fraction of winning trades |
| `profit_factor` | `float` | Gross profit / gross loss |
| `avg_win` | `float` | Average winning trade |
| `avg_loss` | `float` | Average losing trade |
| `avg_win_loss_ratio` | `float` | Average win over average loss |
| `avg_trade_duration_ns` | `int` | Mean trade holding time (ns) |
| `median_trade_duration_ns` | `int` | Median trade holding time (ns) |
| `max_trade_duration_ns` | `int` | Longest trade holding time (ns) |
| `sharpe_ratio` | `float` | Annualized Sharpe ratio |
| `sortino_ratio` | `float` | Annualized Sortino ratio |
| `calmar_ratio` | `float` | Calmar ratio |
| `time_weighted_return` | `float` | Time-weighted return |
| `return_pct` | `float` | Net return percentage |
| `start_time_ns` | `int` | First fill timestamp |
| `end_time_ns` | `int` | Last fill timestamp |

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

### PyEquityPoint

| Field | Type | Description |
|-------|------|-------------|
| `timestamp_ns` | `int64` | Sample timestamp (ns) |
| `equity` | `float64` | Account equity |
| `drawdown_pct` | `float64` | Drawdown from peak (percentage) |

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
