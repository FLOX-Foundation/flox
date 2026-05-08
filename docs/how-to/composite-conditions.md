# Compose multi-symbol multi-TF entry conditions declaratively

Multi-symbol multi-TF strategies often look like a tangle of nested `if` statements that pull values from per-(symbol, timeframe) caches and check warmup at every step. The composite-condition DSL builds the same logic as a tree of indicator handles and operators, so the boolean state machine is the readable structure.

The DSL pulls bars from the per-(symbol, timeframe) ring populated by `Strategy.last_n_closed_bars` (W1-T026). Warmup is uniform: a condition reports `is_ready() = False` until every leaf in the tree has its window of bars.

## Quick start

```python
from flox_py import Strategy
from flox_py.composite import when, TIME_BARS

H4_NS = 4 * 3600 * 1_000_000_000
M5_NS = 5 * 60 * 1_000_000_000

class TrendFollow(Strategy):
    def setup(self):
        self.entry = (
            when(self, btc_id, TIME_BARS, H4_NS).ema(50)
            > when(self, btc_id, TIME_BARS, H4_NS).ema(200)
        ) & (
            when(self, btc_id, TIME_BARS, M5_NS).rsi(14) < 30
        )

    def on_bar(self, ctx, bar):
        if self.entry.is_ready() and self.entry.value():
            self.emit_market_buy(ctx.symbol_id, 0.01)
```

## Building blocks

- `when(strategy, symbol_id, bar_type, param)` returns a handle bound to that timeframe.
- `.sma(period)`, `.ema(period)`, `.rsi(period)`, `.close()` return indicator nodes that compute lazily from the ring.
- Comparison operators (`<`, `<=`, `>`, `>=`, `==`, `!=`) on a node produce a `Condition`.
- Conditions compose with `&` (and), `|` (or), `~` (not).

## Cross-symbol pair trade

```python
self.spread_short = (
    when(self, btc_id, TIME_BARS, H1_NS).close()
    > 1.05 * when(self, eth_id, TIME_BARS, H1_NS).sma(20)
)
```

## Warmup contract

A node returns `nan` for `.value()` when it does not yet have the bars it needs. The condition wrapping it returns `False` for `.value()` if a leaf is not ready, but the strategy should guard with `.is_ready()` to avoid acting on a partial state.

## Performance

The DSL is pure Python and re-evaluates lazily on every `.value()` call. For multi-symbol multi-TF strategies that fire at the rate the engine dispatches bars (per minute, per hour) this is comfortably fast. For per-tick high-frequency code paths, push the comparison logic into the strategy class directly.

## See also

- [Multi-TF context helpers](multi-tf-context.md). The `last_closed_bar` / `last_n_closed_bars` accessors the DSL is built on.
- [Multi-symbol indicators](multi-symbol-indicators.md). When the same indicator runs on every symbol.
