# Writing Your First Codon Strategy

This tutorial walks through creating an SMA crossover strategy in Codon.
The strategy buys when a fast SMA crosses above a slow SMA, and sells on the
reverse crossover.

## Prerequisites

1. Flox built with C API enabled
2. Codon compiler installed

```bash
cmake -B build -DFLOX_ENABLE_CAPI=ON -DFLOX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Step 1: Create the Strategy File

Create `my_sma_strategy.codon`:

```python
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData
from flox.indicators import StreamingSMA


class SmaCrossover(Strategy):
    fast_sma: StreamingSMA
    slow_sma: StreamingSMA
    order_size: float
    long_position: bool

    def __init__(self, symbols: List[int], fast: int = 10, slow: int = 30):
        super().__init__(symbols)
        self.fast_sma = StreamingSMA(fast)
        self.slow_sma = StreamingSMA(slow)
        self.order_size = 1.0
        self.long_position = False

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        price = trade.price.to_double()
        fast = self.fast_sma.update(price)
        slow = self.slow_sma.update(price)

        # Wait until both SMAs have enough data
        if not self.slow_sma.ready:
            return

        sym = self.primary_symbol

        # Golden cross: fast crosses above slow
        if fast > slow and not self.long_position:
            self.emit_market_buy(sym, self.order_size)
            self.long_position = True

        # Death cross: fast crosses below slow
        elif fast < slow and self.long_position:
            self.emit_market_sell(sym, self.order_size)
            self.long_position = False

    def on_start(self):
        print("Strategy started!")

    def on_stop(self):
        pos = self.position()
        print(f"Strategy stopped. Final position: {pos}")
```

## Step 2: Compile to Native Binary

```bash
codon build -exe \
  -o my_sma_strategy \
  -L build/src/capi \
  -lflox_capi \
  my_sma_strategy.codon
```

## Step 3: Understanding the Code

### Imports

- `Strategy` -- base class providing emit/query methods
- `SymbolContext` -- per-symbol state (position, book, prices)
- `TradeData` -- trade event with price, quantity, side
- `StreamingSMA` -- O(1) streaming SMA (pure Codon, no FFI)

### Key Patterns

1. **Override `on_trade`** to receive trade events
2. **Use streaming indicators** for per-tick computation
3. **Call `emit_*` methods** to submit orders
4. **Query `position()`** to check current state

### Performance

Because Codon compiles to native code:
- `on_trade` is compiled to a native function, not interpreted
- `StreamingSMA.update()` is a native function call
- No GIL, no interpreter, no garbage collector pauses

## Step 4: Compare with C++ and Python

The same strategy in C++:

```cpp
void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override {
    prices_.push_back(ev.trade.price.toDouble());
    // ... SMA logic ...
    if (fast_above && !long_position_)
        emitMarketBuy(symbol(), orderSize_);
}
```

And in Python:

```python
def on_trade(self, ctx, trade):
    self.prices.append(trade.price)
    # ... SMA logic ...
    if fast > slow and not self.long_position:
        self.emit_market_buy(ctx.symbol_id, 1.0)
```

All three produce identical trading behavior with the same API shape.

## Next Steps


- See [Indicators Reference](../reference/codon/indicators.md) for all available indicators
- See [Strategy Classes](../how-to/strategy-classes.md) for the full API reference
