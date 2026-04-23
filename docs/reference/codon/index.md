# Codon API Reference

Flox Codon bindings provide a Python-like API compiled to native code via the
[Codon compiler](https://github.com/exaloop/codon).

## Modules

| Module | Description |
|--------|-------------|
| [`flox.strategy`](strategy.md) | Strategy base class for event-driven strategies |
| [`flox.types`](types.md) | Core types: Price, Quantity, TradeData, SymbolContext |
| [`flox.indicators`](indicators.md) | Technical indicators (batch and streaming) |
| [`flox.runner`](runner.md) | Runner, BacktestRunner, Signal |
| [`flox.backtest`](backtest.md) | SimulatedExecutor, BacktestResult, BacktestStats, Engine, SignalBuilder |
| [`flox.tools`](tools.md) | Order books, position tracking, profiles, data I/O, statistics, segment ops |

## Quick Start

```python
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData

class MyStrategy(Strategy):
    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        if trade.price.to_double() > 100.0:
            self.emit_market_buy(self._symbols[0], 1.0)
```

Compile with:
```bash
codon build -exe -o my_strategy -lflox_capi my_strategy.codon
```

## Architecture

Codon strategies call the C API (`libflox_capi.so`) via Codon's C FFI.
Strategy callbacks are compiled to native code via Codon's C FFI.

See [How-To: Codon Bindings](../../how-to/codon-bindings.md) for build instructions.
