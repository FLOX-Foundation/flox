# lrvx Codon Bindings

High-performance trading strategies in Python-like syntax, compiled to native code.

## Overview

Codon compiles Python-like code to native binaries via LLVM.
lrvx Codon bindings provide an event-driven Strategy base class that
mirrors the C++ `lrvx::Strategy` interface.

## Quick Start

```python
from lrvx.strategy import Strategy
from lrvx.context import SymbolContext
from lrvx.types import TradeData
from lrvx.indicators import StreamingEMA

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
# Build lrvx with C API
cmake -B build -DLRVX_BUILD_CAPI=ON -DLRVX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Compile Codon strategy
codon build -exe -o my_strategy -L build/src/capi -llrvx_capi my_strategy.codon
```

## Modules

| Module | Description |
|--------|-------------|
| `lrvx.strategy` | Strategy base class |
| `lrvx.types` | Price, Quantity, TradeData |
| `lrvx.context` | SymbolContext (position, book queries) |
| `lrvx.indicators` | Batch and streaming indicators |

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
