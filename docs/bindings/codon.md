# Codon Bindings

Codon is an ahead-of-time compiled Python-like language that produces native code.
Flox provides Codon bindings via the C API, so strategies compile to binaries with no interpreter overhead.

## Prerequisites

- [Codon compiler](https://github.com/exaloop/codon) installed
- Flox built with `-DFLOX_ENABLE_CAPI=ON`

## Build

```bash
cmake -B build \
  -DFLOX_ENABLE_BACKTEST=ON \
  -DFLOX_ENABLE_CAPI=ON \
  -DFLOX_ENABLE_CODON=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Produces `build/src/capi/libflox_capi.so` and the example binaries in `build/codon/`.

Compile a strategy:

```bash
codon build -exe \
  -o my_strategy \
  -L build/src/capi \
  -lflox_capi \
  my_strategy.codon
```

## Symbols

```python
from flox.runner import Runner, flox_registry_create, flox_registry_add_symbol

reg = flox_registry_create()
btc = flox_registry_add_symbol(reg, "binance".c_str(), "BTCUSDT".c_str(), 0.01)
# btc is a u32 symbol ID — pass it anywhere a symbol is expected
```

## Writing a Strategy

Subclass `Strategy` and override `on_trade`. Order methods default to the primary symbol.

```python
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData
from flox.indicators import StreamingSMA

class SMAcross(Strategy):
    fast: StreamingSMA
    slow: StreamingSMA

    def __init__(self, symbols: List[int]):
        super().__init__(symbols)
        self.fast = StreamingSMA(10)
        self.slow = StreamingSMA(30)

    def on_start(self):
        print("started")

    def on_stop(self):
        print("stopped")

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        f = self.fast.update(trade.price.to_double())
        s = self.slow.update(trade.price.to_double())
        if not self.slow.ready:
            return
        if f > s and self.position() == 0.0:
            self.market_buy(0.01)
        elif f < s and self.position() > 0.0:
            self.close_position()
```

### Order methods

| Method | Description |
|--------|-------------|
| `market_buy(qty)` | Market buy (primary symbol) |
| `market_sell(qty)` | Market sell |
| `limit_buy(price, qty)` | Limit buy (GTC) |
| `limit_sell(price, qty)` | Limit sell (GTC) |
| `stop_market(side, trigger, qty)` | Stop market. `side`: `"buy"` / `"sell"` |
| `take_profit_market(side, trigger, qty)` | Take-profit market |
| `trailing_stop(side, offset, qty)` | Trailing stop by offset |
| `close_position()` | Reduce-only close |
| `cancel_order(order_id)` | Cancel by ID |
| `cancel_all_orders()` | Cancel all |

All methods accept an optional `symbol` kwarg to target a specific symbol by name.

### Context queries

| Method | Description |
|--------|-------------|
| `position()` | Current position (float) |
| `last_price()` | Last trade price |
| `best_bid()` | Best bid |
| `best_ask()` | Best ask |
| `mid_price()` | Mid price |

## Runner (live)

```python
from flox.runner import Runner

def on_signal(sig):
    # sig.side: "buy" | "sell"
    # sig.order_type: "market" | "limit" | ...
    # sig.quantity, sig.price
    print(sig.side, sig.quantity, "@", sig.price)

runner = Runner(reg, on_signal)           # synchronous
# runner = Runner(reg, on_signal, True)   # Disruptor background thread

runner.add_strategy(SMAcross([int(btc)]))
runner.start()

# Feed market data:
runner.on_trade(int(btc), price, qty, True, ts_ns)
runner.on_book_snapshot(int(btc), bid_prices, bid_qtys, ask_prices, ask_qtys, ts_ns)

runner.stop()
```

## BacktestRunner

```python
from flox.runner import BacktestRunner

bt = BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
bt.set_strategy(SMAcross([int(btc)]))

stats = bt.run_csv("/path/to/btcusdt_trades.csv", "BTCUSDT")
print(stats.return_pct, stats.sharpe, stats.max_drawdown_pct)
```

Or pass raw arrays:

```python
stats = bt.run_ohlcv(timestamps_ns, closes, "BTCUSDT")
```

### BacktestStats fields

| Field | Description |
|-------|-------------|
| `return_pct` | Net return percentage |
| `net_pnl` | Net P&L after fees |
| `total_trades` | Round-trip trade count |
| `win_rate` | Winning trade fraction |
| `sharpe` | Annualized Sharpe ratio |
| `sortino` | Annualized Sortino ratio |
| `calmar` | Calmar ratio |
| `max_drawdown_pct` | Peak-to-trough drawdown (%) |
| `profit_factor` | Gross profit / gross loss |
| `final_capital` | Capital at end of backtest |

## Indicators

Streaming indicators with zero FFI overhead (pure Codon):

```python
from flox.indicators import StreamingSMA, StreamingEMA

sma = StreamingSMA(20)
val = sma.update(price)   # returns float; sma.ready is True once primed

ema = StreamingEMA(12)
val = ema.update(price)
```

Batch indicators via C API (`ema`, `sma`, `rsi`, `atr`, `macd`, `bollinger`) — see [Codon Indicators](../reference/codon/indicators.md).

## Full Example

```python
from flox.runner import Runner, BacktestRunner, flox_registry_create, flox_registry_add_symbol
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData
from flox.indicators import StreamingSMA

class SMAcross(Strategy):
    fast: StreamingSMA
    slow: StreamingSMA

    def __init__(self, symbols: List[int]):
        super().__init__(symbols)
        self.fast = StreamingSMA(10)
        self.slow = StreamingSMA(30)

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        f = self.fast.update(trade.price.to_double())
        s = self.slow.update(trade.price.to_double())
        if not self.slow.ready:
            return
        if f > s and self.position() == 0.0:
            self.market_buy(0.01)
        elif f < s and self.position() > 0.0:
            self.close_position()

def on_signal(sig):
    print(f"signal  {sig.side}  qty={sig.quantity:.4f}  [{sig.order_type}]")

def main():
    reg = flox_registry_create()
    btc = flox_registry_add_symbol(reg, "binance".c_str(), "BTCUSDT".c_str(), 0.01)
    btc_id = int(btc)

    # --- Backtest ---
    bt = BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(SMAcross([btc_id]))
    stats = bt.run_csv("btcusdt_trades.csv", "BTCUSDT")
    print(f"Return: {stats.return_pct:.2f}%  Sharpe: {stats.sharpe:.3f}")

    # --- Live ---
    runner = Runner(reg, on_signal)
    runner.add_strategy(SMAcross([btc_id]))
    runner.start()
    # runner.on_trade(btc_id, 67000.0, 0.01, True, 0)
    runner.stop()

main()
```