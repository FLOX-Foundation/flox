# Codon Bindings

Codon is an ahead-of-time compiled Python-like language that produces native binaries.
Flox provides Codon bindings through a C API layer, enabling strategies written in
Python-like syntax to run as compiled native code.

## Prerequisites

- [Codon compiler](https://github.com/exaloop/codon) installed
- Flox built with `-DFLOX_ENABLE_CAPI=ON`

## Building

```bash
cmake -B build \
  -DFLOX_ENABLE_BACKTEST=ON \
  -DFLOX_ENABLE_CAPI=ON \
  -DFLOX_ENABLE_CODON=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

The build produces:

- `build/src/capi/libflox_capi.so` -- shared library for C API
- `build/codon/codon_sma_crossover` -- compiled SMA crossover example
- `build/codon/codon_pairs_strategy` -- compiled pairs trading example

## Writing a Codon Strategy

```python
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData
from flox.indicators import StreamingSMA

class MyStrategy(Strategy):
    fast_sma: StreamingSMA
    slow_sma: StreamingSMA

    def __init__(self, symbols: List[int]):
        super().__init__(symbols)
        self.fast_sma = StreamingSMA(10)
        self.slow_sma = StreamingSMA(30)

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        price = trade.price.to_double()
        fast = self.fast_sma.update(price)
        slow = self.slow_sma.update(price)

        if not self.slow_sma.ready:
            return

        sym = self._symbols[0]
        if fast > slow and ctx.is_flat():
            self.emit_market_buy(sym, 1.0)
        elif fast < slow and ctx.is_long():
            self.emit_close_position(sym)
```

## Compiling a Strategy

```bash
codon build -exe \
  -o my_strategy \
  -L build/src/capi \
  -lflox_capi \
  my_strategy.codon
```

## API Compatibility

Codon strategies use the **same API** as Python strategies:

- `on_trade(ctx, trade)` / `on_book_update(ctx)`
- `emit_market_buy(symbol, qty)` / `emit_limit_sell(symbol, price, qty)`
- `position(symbol)` / `ctx(symbol)`
- `emit_cancel(order_id)` / `emit_close_position(symbol)`

See [Strategy Classes](strategy-classes.md) for the full API reference.

## Available Indicators

Batch indicators (via C API):
`ema`, `sma`, `rsi`, `atr`, `macd`, `bollinger`

Streaming indicators (pure Codon, zero FFI overhead):
`StreamingEMA`, `StreamingSMA`

See [Codon Indicators](../reference/codon/indicators.md) for details.
