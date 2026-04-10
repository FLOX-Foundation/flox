# Indicator graph

When multiple strategies use the same indicator (e.g. ATR used by both ADX and a normalized slope), `IndicatorGraph` computes it once and caches the result.

## Setup

```cpp
#include "flox/indicator/indicator_pipeline.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/slope.h"

using namespace flox;
using namespace flox::indicator;

IndicatorGraph g;
g.setBars(symbolId, bars);
```

## Registering nodes

Each node has a name, a list of dependencies, and a compute function.

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
    for (size_t i = 0; i < slope.size(); ++i) {
        out[i] = (atr[i] > 0 && !std::isnan(slope[i])) ? slope[i] / atr[i] : 0.0;
    }
    return out;
});
```

## Computing

`require()` resolves dependencies recursively. Only computes what's needed.

```cpp
auto& result = g.require(symbolId, "norm_slope");
// atr14 and ema50 computed automatically before norm_slope
```

Already-computed nodes are returned from cache. Circular dependencies throw `std::logic_error`.

## Invalidation

When new data arrives, call `invalidate()` to clear cached results for a symbol.

```cpp
g.setBars(symbolId, newBars);  // also invalidates
// next require() recomputes everything
```

## Multi-symbol

The graph is per-symbol. Different symbols have independent caches.

```cpp
g.setBars(0, btcBars);
g.setBars(1, ethBars);

auto& btcSlope = g.require(0, "norm_slope");
auto& ethSlope = g.require(1, "norm_slope");
// computed independently, cached separately
```
