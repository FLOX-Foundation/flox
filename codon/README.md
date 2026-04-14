# Flox Codon Bindings

High-performance trading strategies in Python-like syntax, compiled to native code.

## Overview

Codon compiles Python-like code to native binaries via LLVM.
Flox Codon bindings provide an event-driven Strategy base class that
mirrors the C++ `flox::Strategy` interface.

## Quick Start

```python
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData
from flox.indicators import StreamingEMA

class MyStrategy(Strategy):
    ema: StreamingEMA

    def __init__(self, symbols: List[int]):
        super().__init__(symbols)
        self.ema = StreamingEMA(20)

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        value = self.ema.update(trade.price.to_double())
        if self.ema.ready and value > trade.price.to_double():
            self.emit_market_buy(self.primary_symbol, 1.0)
```

## Build

```bash
# Build Flox with C API
cmake -B build -DFLOX_ENABLE_CAPI=ON -DFLOX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Compile Codon strategy
codon build -exe -o my_strategy -L build/src/capi -lflox_capi my_strategy.codon
```

## Modules

| Module | Description |
|--------|-------------|
| `flox.strategy` | Strategy base class |
| `flox.types` | Price, Quantity, TradeData |
| `flox.context` | SymbolContext (position, book queries) |
| `flox.indicators` | Batch and streaming indicators |

## Examples

- `examples/sma_crossover.codon` -- SMA crossover strategy
- `examples/pairs_strategy.codon` -- Pairs trading with z-score

## API

The API is identical to the Python Strategy class:

```python
# Callbacks (override)
on_trade(ctx, trade)
on_book_update(ctx)
on_start() / on_stop()

# Signal emission
emit_market_buy(symbol, qty)
emit_market_sell(symbol, qty)
emit_limit_buy(symbol, price, qty)
emit_limit_sell(symbol, price, qty)
emit_cancel(order_id)
emit_close_position(symbol)

# Queries
position(symbol)
ctx(symbol)
```
