# Backtest components

```codon
from flox.backtest import SimulatedExecutor, BacktestResult, BacktestStats
from flox.backtest import SLIPPAGE_NONE, SLIPPAGE_FIXED_TICKS, SLIPPAGE_FIXED_BPS, SLIPPAGE_VOLUME_IMPACT
from flox.backtest import QUEUE_NONE, QUEUE_TOB, QUEUE_FULL
from flox.engine import Engine, SignalBuilder
```

---

## BacktestStats

Returned by `BacktestRunner.run_csv()` / `run_ohlcv()` and `BacktestResult.stats()`.

| Field | Type | Description |
|-------|------|-------------|
| `total_trades` | `int` | Round-trip trade count |
| `winning_trades` | `int` | Winning trades |
| `losing_trades` | `int` | Losing trades |
| `max_consecutive_wins` | `int` | Max consecutive wins |
| `max_consecutive_losses` | `int` | Max consecutive losses |
| `initial_capital` | `float` | Starting capital |
| `final_capital` | `float` | Ending capital |
| `total_pnl` | `float` | Gross P&L |
| `total_fees` | `float` | Total fees paid |
| `net_pnl` | `float` | Net P&L after fees |
| `gross_profit` | `float` | Sum of winning trades |
| `gross_loss` | `float` | Sum of losing trades |
| `max_drawdown` | `float` | Max drawdown (absolute) |
| `max_drawdown_pct` | `float` | Max drawdown (%) |
| `win_rate` | `float` | Winning trade ratio |
| `profit_factor` | `float` | Gross profit / gross loss |
| `avg_win` | `float` | Average winning trade |
| `avg_loss` | `float` | Average losing trade |
| `avg_win_loss_ratio` | `float` | `avg_win / avg_loss` |
| `avg_trade_duration_ns` | `float` | Average trade duration (ns) |
| `median_trade_duration_ns` | `float` | Median trade duration (ns) |
| `max_trade_duration_ns` | `float` | Longest trade (ns) |
| `sharpe` | `float` | Annualized Sharpe ratio |
| `sortino` | `float` | Sortino ratio |
| `calmar` | `float` | Calmar ratio |
| `time_weighted_return` | `float` | Time-weighted return |
| `return_pct` | `float` | Net return (%) |
| `start_time_ns` | `int` | Backtest start timestamp (ns) |
| `end_time_ns` | `int` | Backtest end timestamp (ns) |

---

## SimulatedExecutor

Fills orders from simulated market data. Use directly when you need more control than `BacktestRunner` provides.

```codon
from flox.backtest import SimulatedExecutor, SLIPPAGE_FIXED_BPS, QUEUE_TOB

exec = SimulatedExecutor()
exec.set_default_slippage(SLIPPAGE_FIXED_BPS, bps=2.0)
exec.set_queue_model(QUEUE_TOB, depth=1)

exec.submit_order(order_id, "buy", price, qty)
exec.on_bar(symbol, close_price)
exec.on_trade(symbol, price, is_buy)
exec.advance_clock(ts_ns)
```

### Constructor

```codon
SimulatedExecutor()
```

### Methods

| Method | Description |
|--------|-------------|
| `submit_order(id, side, price, qty, order_type="market", symbol=1)` | Submit an order (`side`: `"buy"`/`"sell"`, `order_type`: `"market"`/`"limit"`) |
| `cancel_order(order_id)` | Cancel an order |
| `cancel_all(symbol)` | Cancel all orders for a symbol |
| `on_bar(symbol, close_price)` | Feed a bar close |
| `on_trade(symbol, price, is_buy)` | Feed a trade |
| `on_trade_qty(symbol, price, qty, is_buy)` | Feed a trade with quantity (enables queue-fill simulation) |
| `on_best_levels(symbol, bid_price, bid_qty, ask_price, ask_qty)` | Feed top-of-book snapshot |
| `advance_clock(timestamp_ns)` | Advance simulated time |
| `set_default_slippage(model, ticks=0, tick_size=0.0, bps=0.0, impact_coeff=0.0)` | Configure slippage for all symbols |
| `set_symbol_slippage(symbol, model, ticks=0, tick_size=0.0, bps=0.0, impact_coeff=0.0)` | Per-symbol slippage override |
| `set_queue_model(model, depth=1)` | Configure limit order queue simulation |
| `close()` | Free resources |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `fill_count` | `int` | Number of fills generated |

### Slippage models

| Constant | Value | Description |
|----------|-------|-------------|
| `SLIPPAGE_NONE` | `0` | No slippage |
| `SLIPPAGE_FIXED_TICKS` | `1` | Fixed tick count per fill |
| `SLIPPAGE_FIXED_BPS` | `2` | Fixed basis points per fill |
| `SLIPPAGE_VOLUME_IMPACT` | `3` | Volume-proportional impact |

### Queue models

| Constant | Value | Description |
|----------|-------|-------------|
| `QUEUE_NONE` | `0` | Fill limit orders immediately at price |
| `QUEUE_TOB` | `1` | Fill only when price trades through level |
| `QUEUE_FULL` | `2` | Model queue position; fill as volume passes |

---

## BacktestResult

Aggregates fills from a `SimulatedExecutor` into statistics and an equity curve.

```codon
from flox.backtest import BacktestResult

result = BacktestResult(initial_capital=10_000.0, fee_rate=0.0004)
result.ingest_executor(exec)
stats = result.stats()
```

### Constructor

```codon
BacktestResult(
    initial_capital: float = 100000.0,
    fee_rate: float = 0.0001,
    use_percentage_fee: bool = True,
    fixed_fee_per_trade: float = 0.0,
    risk_free_rate: float = 0.0,
    annualization_factor: float = 252.0
)
```

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `record_fill(order_id, symbol, side, price, qty, timestamp_ns)` | `None` | Record a single fill |
| `ingest_executor(executor)` | `None` | Drain all fills from a `SimulatedExecutor` |
| `stats()` | `BacktestStats` | Compute and return statistics |
| `equity_curve_size()` | `int` | Number of equity curve points |
| `write_equity_curve_csv(path)` | `bool` | Write equity curve to CSV |
| `close()` | `None` | Free resources |

---

## Engine

Bulk backtesting engine. Loads OHLCV data once, then runs a `SignalBuilder` against it.

```codon
from flox.engine import Engine, SignalBuilder

engine = Engine(initial_capital=10_000.0, fee_rate=0.0004)
engine.load_csv("data/btcusdt.csv", symbol="BTCUSDT")

signals = SignalBuilder()
close = engine.close()
for i in range(1, len(close)):
    if close[i] > close[i-1]:
        signals.buy(engine.ts()[i], 0.01, "BTCUSDT")
    else:
        signals.sell(engine.ts()[i], 0.01, "BTCUSDT")

stats = engine.run(signals)
```

### Constructor

```codon
Engine(initial_capital: float = 100000.0, fee_rate: float = 0.0001)
```

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `load_csv(path, symbol="")` | `None` | Load OHLCV CSV (columns: timestamp, open, high, low, close, volume) |
| `load_ohlcv(timestamps, opens, highs, lows, closes, volumes, symbol="")` | `None` | Load raw OHLCV arrays |
| `bar_count(symbol="")` | `int` | Number of bars loaded |
| `run(signals, default_symbol="")` | `BacktestStats` | Run a `SignalBuilder` and return statistics |

### Data accessors

| Method | Returns | Description |
|--------|---------|-------------|
| `ts(symbol="")` | `List[int]` | Timestamps (ns) |
| `open(symbol="")` | `List[float]` | Open prices |
| `high(symbol="")` | `List[float]` | High prices |
| `low(symbol="")` | `List[float]` | Low prices |
| `close(symbol="")` | `List[float]` | Close prices |
| `volume(symbol="")` | `List[float]` | Volumes |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `symbols` | `List[str]` | Registered symbol names |

---

## SignalBuilder

Builds a signal list to pass to `Engine.run()`.

```codon
signals = SignalBuilder()
signals.buy(ts_ms, 0.01, "BTCUSDT")
signals.sell(ts_ms, 0.01)
stats = engine.run(signals)
```

### Methods

| Method | Description |
|--------|-------------|
| `buy(ts_ms, qty, symbol="")` | Add a market long entry at `ts_ms` |
| `sell(ts_ms, qty, symbol="")` | Add a market short entry at `ts_ms` |
| `limit_buy(ts_ms, price, qty, symbol="")` | Add a limit long entry |
| `limit_sell(ts_ms, price, qty, symbol="")` | Add a limit short entry |
| `clear()` | Clear all signals |
| `__len__()` | Number of signals |
