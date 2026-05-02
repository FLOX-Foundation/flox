# Indicator graph

When multiple strategies use the same indicator (e.g. ATR used by both ADX and a normalised slope), `IndicatorGraph` computes it once and caches the result. Available in C++, Python, and Node.js.

## Concept

You define **nodes** by name. Each node has a list of upstream dependencies and a compute function. `require()` resolves the DAG, computes only what's needed, and caches the result. `invalidate()` clears the cache when new data arrives.

The graph is per-symbol; different symbols have independent caches.

## Setup

=== "Python"

    ```python
    import flox_py as flox

    g = flox.IndicatorGraph()
    g.set_bars(symbol_id, bars)   # bars is a structured numpy array
    ```

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');
    const g = new flox.IndicatorGraph();
    g.setBars(symbolId, bars);
    ```

=== "C++"

    ```cpp
    #include "flox/indicator/indicator_pipeline.h"
    #include "flox/indicator/{ema,atr,slope}.h"

    using namespace flox::indicator;
    IndicatorGraph g;
    g.setBars(symbolId, bars);
    ```

## Registering nodes

=== "Python"

    ```python
    g.add_node("atr14", deps=[],
               factory=lambda: flox.ATR(14))            # uses bar high/low/close

    g.add_node("ema50", deps=[],
               factory=lambda: flox.EMA(50))            # uses bar close

    # Compute "norm_slope" from ema50 and atr14
    def norm_slope(ema, atr):
        slope = flox.Slope(1).compute(ema)
        out = slope / atr            # numpy element-wise; NaN-safe
        out[atr <= 0] = 0
        return out

    g.add_node("norm_slope", deps=["ema50", "atr14"], factory=norm_slope)
    ```

=== "Node.js"

    ```javascript
    g.addNode("atr14", { source: "ohlc" }, () => new flox.ATR(14));
    g.addNode("ema50", { source: "close" }, () => new flox.EMA(50));
    g.addNode("normSlope", { deps: ["ema50", "atr14"] }, ({ ema50, atr14 }) => {
      const slope = new flox.Slope(1).compute(ema50);
      return slope.map((s, i) => (atr14[i] > 0 ? s / atr14[i] : 0));
    });
    ```

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

`require()` resolves dependencies recursively and only computes what's needed. Already-computed nodes are returned from cache. Circular dependencies are detected and raise an error.

=== "Python"

    ```python
    out = g.require(symbol_id, "norm_slope")    # ema50 + atr14 computed first
    ```

=== "Node.js"

    ```javascript
    const out = g.require(symbolId, "normSlope");
    ```

=== "C++"

    ```cpp
    auto& result = g.require(symbolId, "norm_slope");
    ```

## Invalidation

When new data arrives, call `set_bars` (or `invalidate`) and the next `require()` recomputes from scratch.

=== "Python"

    ```python
    g.set_bars(symbol_id, new_bars)
    ```

=== "C++"

    ```cpp
    g.setBars(symbolId, newBars);
    ```

## Multi-symbol

The graph is per-symbol. Each symbol has its own cache; different symbols are independent.

=== "Python"

    ```python
    g.set_bars(0, btc_bars)
    g.set_bars(1, eth_bars)
    btc_slope = g.require(0, "norm_slope")
    eth_slope = g.require(1, "norm_slope")
    ```

=== "C++"

    ```cpp
    g.setBars(0, btcBars);
    g.setBars(1, ethBars);
    auto& btcSlope = g.require(0, "norm_slope");
    auto& ethSlope = g.require(1, "norm_slope");
    ```

See also: [Multi-symbol indicators](multi-symbol-indicators.md).
