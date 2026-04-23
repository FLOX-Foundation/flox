# Runner, LiveEngine, BacktestRunner

```codon
from flox.runner import Runner, BacktestRunner
```

---

## Signal

Emitted by a strategy when it places an order.

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `int` | Order ID |
| `symbol` | `int` | Symbol ID |
| `side` | `str` | `"buy"` or `"sell"` |
| `order_type` | `str` | `"market"`, `"limit"`, `"stop_market"`, `"stop_limit"`, `"tp_market"`, `"tp_limit"`, `"trailing_stop"`, `"cancel"`, `"cancel_all"`, `"modify"` |
| `price` | `float` | Limit price (0 for market orders) |
| `quantity` | `float` | Order quantity |
| `trigger_price` | `float` | Stop/take-profit trigger |
| `trailing_offset` | `float` | Trailing stop absolute offset |
| `trailing_bps` | `int` | Trailing stop callback rate (basis points) |
| `new_price` | `float` | Modify: updated price |
| `new_quantity` | `float` | Modify: updated quantity |

---

## Runner

Synchronous strategy host. Push market data; strategy callbacks fire before the call returns. Pass `threaded=True` to use a Disruptor ring buffer with strategy callbacks running in a background C++ thread.

```codon
from flox.runner import Runner

def on_signal(sig: Signal):
    print(sig.side, sig.quantity, sig.price)

runner = Runner(registry, on_signal)          # synchronous
runner = Runner(registry, on_signal, True)    # threaded

runner.add_strategy(my_strategy)
runner.start()
runner.on_trade(btc, 67000.0, 0.01, True, ts_ns)
runner.on_book_snapshot(btc, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)
runner.stop()
```

### Constructor

```codon
Runner(registry: cobj, on_signal: Function[[Signal], None], threaded: bool = False)
```

| Parameter | Description |
|-----------|-------------|
| `registry` | Handle from `flox_registry_create()` |
| `on_signal` | Called when a strategy emits an order |
| `threaded` | If `True`, use Disruptor-based consumer thread |

### Methods

| Method | Description |
|--------|-------------|
| `add_strategy(strategy)` | Register a strategy instance |
| `start()` | Start the runner |
| `stop()` | Stop and clean up |
| `on_trade(symbol, price, qty, is_buy, ts_ns)` | Push a trade tick |
| `on_book_snapshot(symbol, bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)` | Push a full L2 snapshot |

`symbol` accepts a symbol ID (`int`) or any value with a `symbol_id` property.

---

## BacktestRunner

Replays OHLCV data through a strategy. Emitted orders go to `SimulatedExecutor` automatically.

```codon
from flox.runner import BacktestRunner

bt = BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000.0)
bt.set_strategy(my_strategy)

stats = bt.run_csv("data/btcusdt.csv", "BTCUSDT")
stats = bt.run_ohlcv(timestamps, closes, "BTCUSDT")
print(stats.return_pct, stats.sharpe)
```

### Constructor

```codon
BacktestRunner(registry: cobj, fee_rate: float = 0.0004, initial_capital: float = 10_000.0)
```

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `set_strategy(strategy)` | `None` | Attach a strategy |
| `run_csv(path, symbol)` | `BacktestStats` | Replay a CSV file (columns: timestamp, open, high, low, close, volume) |
| `run_ohlcv(timestamps, closes, symbol)` | `BacktestStats` | Replay raw arrays (`List[int]` timestamps in ns, `List[float]` closes) |
| `close()` | `None` | Free resources |

See [Backtest](backtest.md) for the `BacktestStats` field reference.
