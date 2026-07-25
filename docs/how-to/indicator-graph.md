# Indicator graph

When multiple strategies use the same indicator (e.g. ATR used by both ADX and a normalised slope), `IndicatorGraph` computes it once and caches the result. Available in C++, Python, and Node.js.

## Concept

You define **nodes** by name. Each node has a list of upstream dependency names and a compute function `(graph, symbol) -> array`. `require()` resolves the DAG, computes only what's needed, and caches the result. `invalidate()` clears the cache when new data arrives.

A node reads its dependencies through `graph.get(symbol, "dep_name")` — dependencies are not passed as function arguments. They are guaranteed to be computed before the node runs.

The graph is per-symbol; different symbols have independent caches.

## Setup

Bars are supplied as plain float64 arrays, not a structured array. Only `close` is required; `high`, `low` and `volume` default to `close` (volume to zero) when omitted.

=== "Python"

    ```python
    import flox_py as flox
    import numpy as np

    g = flox.IndicatorGraph()
    g.set_bars(symbol_id, close, high, low, volume)   # high/low/volume optional
    ```

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');
    const g = new flox.IndicatorGraph();
    g.setBars(symbolId, close, high, low, volume);   // Float64Array; high/low/volume optional
    ```

=== "C++"

    ```cpp
    #include "flox/indicator/indicator_pipeline.h"

    using namespace flox::indicator;
    IndicatorGraph g;
    g.setBars(symbolId, bars);        // std::span<const flox::Bar>
    ```

## Registering nodes

`add_node(name, deps, fn)` — `deps` is a list of node names, `fn` is called as `fn(graph, symbol)` and returns an array of the same length as the bar series.

=== "Python"

    ```python
    g.add_node("atr14", [],
               lambda graph, sym: flox.atr(graph.high(sym), graph.low(sym),
                                           graph.close(sym), 14))

    g.add_node("ema50", [],
               lambda graph, sym: flox.ema(graph.close(sym), 50))

    def norm_slope(graph, sym):
        atr = graph.get(sym, "atr14")
        slope = flox.slope(graph.get(sym, "ema50"), 1)
        out = np.divide(slope, atr, out=np.zeros_like(slope), where=atr > 0)
        return out

    g.add_node("norm_slope", ["ema50", "atr14"], norm_slope)
    ```

    For a single-input indicator object there is sugar over `add_node`:

    ```python
    g.indicator("ema50", flox.EMA(50), source="close")
    # equivalent to add_node("ema50", [], lambda gr, sym: ind.compute(gr.close(sym)))
    ```

    `indicator()` only fits indicators whose `compute()` takes one series; multi-input
    ones (ATR, CCI, Stochastic) need `add_node`.

=== "Node.js"

    ```javascript
    g.addNode('atr14', [], (graph, sym) =>
      flox.atr(graph.high(sym), graph.low(sym), graph.close(sym), 14));

    g.addNode('ema50', [], (graph, sym) => flox.ema(graph.close(sym), 50));

    g.addNode('normSlope', ['ema50', 'atr14'], (graph, sym) => {
      const atr = graph.get(sym, 'atr14');
      const slope = flox.slope(graph.get(sym, 'ema50'), 1);
      const out = new Float64Array(slope.length);
      for (let i = 0; i < slope.length; ++i) out[i] = atr[i] > 0 ? slope[i] / atr[i] : 0;
      return out;
    });
    ```

    The `IndicatorGraph` declarations in `index.d.ts` are stale — they type `dependencies`
    as node ids and `require`/`get` as taking a node handle. The addon takes node **names**
    and `(symbol, name)`, as above.

=== "C++"

    ```cpp
    g.addNode("atr14", {}, [](IndicatorGraph& g, SymbolId sym) {
        return ATR(14).compute(g.high(sym), g.low(sym), g.close(sym));
    });

    g.addNode("ema50", {}, [](IndicatorGraph& g, SymbolId sym) {
        return EMA(50).compute(g.close(sym));
    });

    g.addNode("norm_slope", {"ema50", "atr14"}, [](IndicatorGraph& g, SymbolId sym) {
        auto& ema = *g.get(sym, "ema50");
        auto& atr = *g.get(sym, "atr14");
        auto slope = Slope(1).compute(ema);
        std::vector<double> out(slope.size());
        for (size_t i = 0; i < slope.size(); ++i)
          out[i] = (atr[i] > 0 && !std::isnan(slope[i])) ? slope[i] / atr[i] : 0.0;
        return out;
    });
    ```

## Computing

`require(symbol, name)` resolves dependencies recursively and only computes what's needed. Already-computed nodes are returned from cache. An unknown node raises `E_GRAPH_001`; a cycle raises `E_GRAPH_002`.

`get(symbol, name)` returns the cached array without triggering a compute — `None` / `null` if the node has not been computed for that symbol yet.

=== "Python"

    ```python
    out = g.require(symbol_id, "norm_slope")    # ema50 + atr14 computed first
    cached = g.get(symbol_id, "ema50")          # None before require ran
    ```

=== "Node.js"

    ```javascript
    const out = g.require(symbolId, 'normSlope');
    const cached = g.get(symbolId, 'ema50');    // null before require ran
    ```

=== "C++"

    ```cpp
    const auto& result = g.require(symbolId, "norm_slope");
    ```

## Invalidation

When new data arrives, call `set_bars` (or `invalidate`) and the next `require()` recomputes from scratch. `invalidate_all()` drops every symbol's cache.

=== "Python"

    ```python
    g.set_bars(symbol_id, new_close)
    g.invalidate(symbol_id)
    g.invalidate_all()
    ```

=== "Node.js"

    ```javascript
    g.setBars(symbolId, newClose);
    g.invalidate(symbolId);
    g.invalidateAll();
    ```

=== "C++"

    ```cpp
    g.setBars(symbolId, newBars);
    g.invalidate(symbolId);
    g.invalidateAll();
    ```

## Streaming

`StreamingIndicatorGraph` appends bars one at a time instead of setting a whole series. `step()` pushes a bar and invalidates that symbol; `current()` returns the latest value of a node (NaN before warm-up or after a reset); `bar_count()` reports how many bars are buffered; `reset()` / `reset_all()` drop the buffered bars.

Node definitions are the same as the batch graph — the compute function still returns the full array, and `current()` takes its last element. A batch graph fed the same closes produces the same value in its last slot.

=== "Python"

    ```python
    sg = flox.StreamingIndicatorGraph()
    sg.add_node("double_close", [], lambda graph, sym: graph.close(sym) * 2.0)

    for c in [10.0, 20.0, 30.0, 40.0, 50.0]:
        sg.step(0, c)

    sg.current(0, "double_close")   # 100.0
    sg.bar_count(0)                 # 5
    sg.reset(0)                     # bar_count 0, current NaN
    ```

    In Python `StreamingIndicatorGraph` is an alias of `IndicatorGraph` — the same
    object has both the batch (`set_bars` / `require`) and the streaming
    (`step` / `current`) methods, and `set_bars` can seed a series that `step` then extends.

=== "Node.js"

    ```javascript
    const sg = new flox.StreamingIndicatorGraph();
    sg.addNode('doubleClose', [], (graph, sym) => {
      const c = graph.close(sym);
      const out = new Float64Array(c.length);
      for (let i = 0; i < c.length; ++i) out[i] = c[i] * 2.0;
      return out;
    });

    for (const c of [10.0, 20.0, 30.0, 40.0, 50.0]) sg.step(0, c);

    sg.current(0, 'doubleClose');   // 100
    sg.barCount(0);                 // 5
    sg.reset(0);                    // barCount 0, current NaN
    ```

    In Node the two classes are separate wrappers: `StreamingIndicatorGraph` has
    `addNode` / `step` / `current` / `barCount` / `reset` / `resetAll` but no `setBars`.

=== "C++"

    ```cpp
    // StreamingIndicatorGraph is an alias of IndicatorGraph.
    g.step(symbolId, bar);
    double v = g.current(symbolId, "norm_slope");   // NaN until warmed up
    g.reset(symbolId);
    ```

## Multi-symbol

The graph is per-symbol. Each symbol has its own cache; different symbols are independent. Nodes are registered once and apply to every symbol.

=== "Python"

    ```python
    g.set_bars(0, btc_close)
    g.set_bars(1, eth_close)
    btc_slope = g.require(0, "norm_slope")
    eth_slope = g.require(1, "norm_slope")
    ```

=== "Node.js"

    ```javascript
    g.setBars(0, btcClose);
    g.setBars(1, ethClose);
    const btcSlope = g.require(0, 'normSlope');
    const ethSlope = g.require(1, 'normSlope');
    ```

=== "C++"

    ```cpp
    g.setBars(0, btcBars);
    g.setBars(1, ethBars);
    const auto& btcSlope = g.require(0, "norm_slope");
    const auto& ethSlope = g.require(1, "norm_slope");
    ```

See also: [Multi-symbol indicators](multi-symbol-indicators.md).
