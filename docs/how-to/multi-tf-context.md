# Read multi-timeframe context from a strategy

Multi-timeframe strategies need the question "what was the most recent closed 4h bar at the moment this 5m bar fired" answered cheaply and correctly. flox now keeps a per-(symbol, timeframe) ring of closed bars on every `Strategy` and exposes two accessors so user code does not need to repeat the bookkeeping.

```python
from flox_py import Strategy

H4 = 4 * 60 * 60 * 1_000_000_000  # nanoseconds
M5 = 5 * 60 * 1_000_000_000
TIME_BARS = 0  # BarType.Time

class TrendFollower(Strategy):
    def on_bar(self, ctx, bar):
        if bar.bar_type_param != M5:
            return
        h4 = self.last_closed_bar(ctx.symbol_id, TIME_BARS, H4)
        if h4 is None:
            return  # warmup
        if h4.close > h4.open:  # 4h trend up
            self.maybe_enter(ctx, bar)
```

## API

- `last_closed_bar(symbol_id, bar_type, param) -> dict | None` returns
  the most recent bar for the given timeframe, or `None` if no bar of
  that timeframe has been emitted yet for the symbol. The dict has
  `open / high / low / close / volume / start_ns / end_ns`.
- `last_n_closed_bars(symbol_id, bar_type, param, n) -> list[dict]`
  returns the most recent up to `n` closed bars in chronological order
  (oldest first). Empty until warmup completes.
- `bar_ring_capacity()` and `set_bar_ring_capacity(n)` control the
  per-(symbol, tf) ring depth. Default is 64; raise it for strategies
  that look further back.

The ring fills automatically as the engine dispatches bars. There is
no explicit register call.

## bar_type values

| Value | Meaning | `param` interpretation |
|------:|---------|-----------------------|
| 0 | Time bar | Interval in nanoseconds. |
| 1 | Tick bar | Tick count. |
| 2 | Volume bar | Volume threshold (`Volume::fromDouble(...)` raw). |
| 3 | Renko bar | Brick size in price-raw. |
| 4 | Range bar | Range in price-raw. |
| 5 | Heikin-Ashi bar | Same as the underlying time interval. |
| 6 | Bps-range bar | Range in basis points × 10. |

## Cross-binding

The Node binding exposes the same surface on the emitter passed into
`onTrade` / `onBar`:

```javascript
function onBar(ctx, bar, emit) {
  const h4 = emit.lastClosedBar(ctx.symbolId, 0, 4 * 3600 * 1_000_000_000);
  if (!h4) return;
  // ... use h4.close, h4.open, ...
}
```

`emit.setBarRingCapacity(n)` adjusts ring depth.

## See also

- [Multi-symbol indicators](multi-symbol-indicators.md). Compose
  per-symbol indicators across the timeframe ring.
- [Bar aggregation](bar-aggregation.md). How bars get produced.
