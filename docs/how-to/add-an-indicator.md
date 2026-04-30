# Add a new indicator

The framework is designed so that adding a new indicator is **one C++ class
plus one line in a registry**. After those two changes it appears in C++,
Python, every binding, in `flox.list_indicators()` discovery, in the parity
tests, and in the docs site, all automatically.

## 1. Write the C++ class

Create `include/flox/indicator/my_indicator.h`. Implement the canonical
`compute()` for your indicator — that's the single source of truth for the
math. Inherit from the matching streaming mixin to get
`update()`/`value()`/`ready()`/`reset()` on the same class for free:

```cpp
#pragma once

#include "flox/indicator/streaming.h"

#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

class MyIndicator : public StreamingSingle<MyIndicator>
{
 public:
  explicit MyIndicator(size_t period) noexcept : _period(period) {}

  std::vector<double> compute(std::span<const double> input) const
  {
    std::vector<double> out(input.size(), std::nan(""));
    // ... your logic ...
    return out;
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::SingleIndicator<flox::indicator::MyIndicator>);
```

The streaming mixin you choose depends on the input shape:

| Input | Mixin |
|---|---|
| Single series (`compute(span)`) | `StreamingSingle<T>` |
| `compute(high, low, close)` | `StreamingBar<T>` |
| `compute(high, low)` | `StreamingHighLow<T>` |
| `compute(open, high, low, close)` | `StreamingOhlc<T>` |
| `compute(x, y)` | `StreamingPair<T>` |
| Multi-output result struct | hand-written streaming methods (see `MACD`, `Bollinger`, `Stochastic`) |

## 2. Register it

Add **one line** to `include/flox/indicator/registry.def`:

```c
FLOX_INDICATOR(MyIndicator, my_indicator, SingleInput, (size_t period))
```

The `Kind` matches the mixin you used (`SingleInput`, `BarInput`,
`HighLowInput`, `OhlcInput`, `PairInput`, or `MultiOutput`).

## 3. Build, regenerate docs, run tests

```sh
cmake --build build
python3 scripts/gen_indicator_docs.py
ctest --test-dir build
```

You now have:

- `flox::indicator::MyIndicator` in C++ with `compute()` (batch) and
  `update()`/`value()`/`ready()`/`reset()` (streaming) on the same instance.
- `flox.MyIndicator(...)` in Python with `compute(arr)` and
  `update(v)` / `value` / `ready` / `reset()` on the same instance.
- `flox.MyIndicator(...)` in **Node.js** — same surface, same semantics
  (thin N-API wrapper around the same C++ class).
- `MyIndicator` in **QuickJS** (`quickjs/flox/indicators.js`) — accumulates
  history and re-runs the batch C-API; same `update`/`value`/`ready`/`reset`.
  Add a 3-line class to that file using one of the `_Stream*` helper bases.
- `MyIndicator` in **Codon** (`codon/flox/indicators.codon`) — same pattern
  as QuickJS, history + batch call.
- The streaming-vs-batch parity test
  (`tests/test_streaming_parity.cpp`) automatically covers it once you add
  one line to the registry-driven test list.
- `flox.list_indicators()` returns it (Python and Node).
- The docs site lists it on every per-binding page (run
  `python3 scripts/gen_indicator_docs.py`).

The parity is **by construction** — `update()` calls your `compute()`
internally, so there is no separate streaming logic to drift. No ring
buffer, no warmup logic, no seeding to debug.

## 4. Reading the existing 22 indicators

Every indicator listed in `registry.def` (EMA, SMA, RSI, ATR, MACD, ...)
follows exactly this pattern. Look at any `include/flox/indicator/<name>.h`
for a working example.

## When NOT to use the streaming mixin

The mixin re-runs `compute()` on the accumulated history each
`update()` — O(N) per tick. Acceptable for research, UI, and
moderate-throughput live trading. For a production-critical hot path you
can specialize `update()` to do O(1) state-keeping, but you **must** keep
the observable behaviour identical to `compute().back()` so the parity
test continues to pass.
