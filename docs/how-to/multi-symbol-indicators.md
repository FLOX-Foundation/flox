# Multi-symbol indicators

Run the same indicator pipeline across multiple symbols in parallel.

## Partitioning

Split a flat bar array or BarEvent stream into per-symbol groups.

```cpp
#include "flox/indicator/multi_symbol.h"
using namespace flox::indicator;

// from BarEvents (carries symbol ID)
auto data = partitionBySymbol(barEvents);

// from parallel arrays
auto data = partitionBySymbol(bars, symbolIds);
```

## Sequential

```cpp
forEachSymbol(data, [](SymbolId sym, std::span<const Bar> bars) {
    auto ema = EMA(50).compute(indicator::close(bars));
    // ...
});
```

## Parallel

Same API, runs on a thread pool. The function must be thread-safe (no shared mutable state).

```cpp
forEachSymbolParallel(data, [](SymbolId sym, std::span<const Bar> bars) {
    auto ema = EMA(50).compute(indicator::close(bars));
    // ...
}, 0);  // 0 = all cores
```
