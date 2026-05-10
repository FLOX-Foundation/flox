# Multi-symbol indicators

> **Looking for `Correlation(BTC, ETH)`?** This page is about
> running the same single-input indicator (SMA, EMA, RSI)
> independently per symbol. For two-input indicators that consume
> a synchronised pair of streams, see
> [cross-symbol-indicators.md](cross-symbol-indicators.md).

Run the same indicator pipeline across multiple symbols. The C++ engine ships a built-in helper for parallel execution; from Python and Node.js, group your data per symbol and either iterate or use a process pool.

## Partition by symbol

=== "Python"

    ```python
    import flox_py as flox

    # Either group manually...
    by_sym = {}
    for ev in bar_events:
        by_sym.setdefault(ev.symbol, []).append(ev)

    # ...or rely on numpy structured arrays already grouped
    btc_bars = bars[bars["symbol"] == 1]
    eth_bars = bars[bars["symbol"] == 2]
    ```

=== "Node.js"

    ```javascript
    const bySym = new Map();
    for (const ev of barEvents) {
      if (!bySym.has(ev.symbol)) bySym.set(ev.symbol, []);
      bySym.get(ev.symbol).push(ev);
    }
    ```

=== "C++"

    ```cpp
    #include "flox/indicator/multi_symbol.h"
    using namespace flox::indicator;

    // from BarEvents (each carries a symbol ID)
    auto data = partitionBySymbol(barEvents);

    // or from parallel arrays
    auto data = partitionBySymbol(bars, symbolIds);
    ```

## Sequential iteration

=== "Python"

    ```python
    for sym_id, bars in by_sym.items():
        ema = flox.EMA(50).compute(np.array([b.close for b in bars]))
        # ...
    ```

=== "Node.js"

    ```javascript
    for (const [sym, bars] of bySym) {
      const closes = Float64Array.from(bars.map(b => b.close));
      const ema = new flox.EMA(50).compute(closes);
      // ...
    }
    ```

=== "C++"

    ```cpp
    forEachSymbol(data, [](SymbolId sym, std::span<const Bar> bars) {
        auto ema = EMA(50).compute(indicator::close(bars));
        // ...
    });
    ```

## Parallel execution

Indicator compute releases the GIL in Python, so threads/processes give real speedup.

=== "Python"

    ```python
    from concurrent.futures import ProcessPoolExecutor

    def compute_one(sym_id_and_bars):
        sym_id, bars = sym_id_and_bars
        return sym_id, flox.EMA(50).compute(np.array([b.close for b in bars]))

    with ProcessPoolExecutor() as pool:
        results = dict(pool.map(compute_one, by_sym.items()))
    ```

=== "Node.js"

    Use [`worker_threads`](https://nodejs.org/api/worker_threads.html) or a tool like `piscina` to fan-out per symbol.

=== "C++"

    ```cpp
    forEachSymbolParallel(data,
        [](SymbolId sym, std::span<const Bar> bars) {
            auto ema = EMA(50).compute(indicator::close(bars));
            // ...
        },
        /*threads=*/0);   // 0 = all cores
    ```

The function passed in must be thread-safe (no shared mutable state). C++ uses an internal pool; Python uses processes (real parallelism without the GIL).

## See also

- [Indicator graph](indicator-graph.md) — caching shared computations across nodes
- [Bar aggregation](bar-aggregation.md) — preparing the bars themselves
