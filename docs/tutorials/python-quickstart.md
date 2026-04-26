# Python quickstart

## Requirements

- Python 3.10+

## 1. Install

```bash
pip install flox-py
```

## 2. First indicator

```python
import flox_py as flox

ema = flox.EMA(20)

prices = [100 + i * 0.1 for i in range(50)]
for price in prices:
    value = ema.update(price)
    if value is not None:
        print(f"EMA(20): {value:.4f}")
```

All streaming indicators return `None` during warmup and a float once ready. Check `.ready` to test without calling update.

## 3. First strategy

```python
import flox_py as flox

registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

class SMACross(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = flox.SMA(10)
        self.slow = flox.SMA(30)

    def on_trade(self, ctx, trade):
        f = self.fast.update(trade.price)
        s = self.slow.update(trade.price)
        if f is None or s is None:
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.01)
        elif f < s and ctx.is_long():
            self.close_position()

def on_signal(sig):
    print(sig.side, sig.order_type, sig.quantity)

runner = flox.Runner(registry, on_signal)
runner.add_strategy(SMACross([btc]))
runner.start()

# Feed market data from your source:
# runner.on_trade(btc, price, qty, is_buy, ts_ns)

runner.stop()
```

## 4. Backtest

```python
bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(SMACross([btc]))

stats = bt.run_csv("btcusdt_trades.csv")
print(stats["return_pct"], stats["sharpe"], stats["max_drawdown_pct"])
```

The CSV format is `timestamp,price,qty,is_buy` — one trade per row.

---

## Building from source

Use this if you need the current `main` branch before a release is published.

### Requirements

- GCC 14+ or Clang 18+
- CMake 3.22+
- Python 3.10+

```bash
git clone https://github.com/FLOX-Foundation/flox.git
cd flox
pip install scikit-build-core pybind11 numpy
pip install ./python
```

That's it — `scikit-build-core` handles the cmake build internally. The module installs into your environment the same way as the PyPI package.

---

## Next steps

- [Indicators reference](../reference/python/indicators.md) — full indicator API
- [Python bindings guide](../bindings/python.md) — batch backtest engine, grid search, live runner
- [Backtesting how-to](../how-to/backtest.md)
