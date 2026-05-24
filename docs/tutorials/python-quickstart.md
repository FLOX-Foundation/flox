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

Two paths. Pick the second by default.

### Bare backtest

```python
bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(SMACross([btc]))

stats = bt.run_csv("btcusdt_trades.csv")
print(stats["return_pct"], stats["sharpe"], stats["max_drawdown_pct"])
```

CSV format: `timestamp,price,qty,is_buy`, one trade per row. Flat
fee rate, no funding, no liquidation, no rate limits, no queue
position. Use for an indicator sanity check, not for a decision
about real capital.

### Realistic backtest

```python
stack = flox.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
acct = stack.account()
liq = stack.liquidation()
fees = stack.fees()
funding = stack.funding()
exec_ = stack.executor()
```

One call wires the cross-margin Account, the MM tier ladder + ADL
ranking, the 30d VIP fee schedule (bound to the account), the
funding settlement schedule, the venue-specific rate-limit policy,
and the venue-availability hook. Other factories: `bybit_linear`,
`okx_swap`, `deribit`. For custom venues see
[`flox.assemble_custom_venue(...)`](../how-to/realistic-backtest.md#fully-custom-venue).

Full pattern: [Realistic backtest in one call](../how-to/realistic-backtest.md).

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

- [Realistic backtest in one call](../how-to/realistic-backtest.md) —
  venue stack with fees, funding, liquidation, rate limits.
- [Paper trading](../how-to/paper-trading.md) — same strategy class
  against a live feed, simulated fills.
- [Connect FLOX to a CCXT exchange](../how-to/ccxt-adapter.md) —
  promote to live.
- [Inspect a tape and run in the replay viewer](../how-to/replay-viewer.md) —
  visualize what your strategy did.
- [Control engine over MCP](../how-to/mcp-control-plane.md) —
  AI agents driving the engine with scoped permission and audit.
- [Indicators reference](../reference/python/indicators.md) — full indicator API.
- [Python bindings guide](../bindings/python.md) — batch backtest engine, grid search, live runner.
- [Backtesting how-to](../how-to/backtest.md).
