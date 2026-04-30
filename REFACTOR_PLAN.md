# Refactor Plan: Single-Source-of-Truth Indicators

**Status:** open work, tracked under issue #111
**Audience:** the engineer (or agent) doing the work end-to-end
**Last updated:** 2026-04-30

---

## Why this exists

Today every indicator has **multiple parallel implementations**:

- `compute()` in C++ core (batch, correct).
- A separate streaming class with its own `update()` / `value()` / `reset()` in
  Python (`PySMA`, `PyEMA`, ...).
- Another in Node (`STREAMING_SINGLE` macros).
- Another in QuickJS (`indicators.js` JS classes).
- Another in Codon.

Result: drift. The parity tests in #121 found 4 real bugs in Python streaming
(PyDEMA, PyTEMA, PyATR, PyMACD) where the warmup/seeding diverged silently from
the batch reference. The same bugs almost certainly exist in Node/QuickJS/Codon
(no parity test there yet).

This refactor eliminates the duplication entirely. After it:

- **One class per indicator**, in C++ core.
- **Bindings are thin wrappers** — only type marshalling, zero logic.
- **Streaming === batch by construction**. Parity is a tautology, not a test.
- **One DAG** (`IndicatorGraph`). `StreamingIndicatorGraph` is a `step()`-shaped
  facade over it (already so).
- **Adding a new indicator = one C++ class + one line in a registry.** It
  appears in every binding, in every test, in the docs site, automatically.

---

## Hard rules — what we will NOT have

If any of these statements becomes true at any point during the refactor, stop
and reconsider — you are doing it wrong.

1. ❌ Two implementations of "EMA" (or any other indicator) anywhere in the
   codebase. The math lives **once**, in `include/flox/indicator/ema.h`.
2. ❌ A `class PySMA` / `STREAMING_SINGLE(SMA, ...)` / `class SMA { update(...) }`
   in any binding layer that holds its own ring buffer / alpha / sum / count.
   Bindings expose, they don't reimplement.
3. ❌ Two DAG types in user-facing API. Today `IndicatorGraph` and
   `StreamingIndicatorGraph` are two separate public classes — that's the same
   "batch/streaming duplication" pattern at the graph level, just one
   abstraction up. **Collapse them.** One `IndicatorGraph` class with both
   `set_bars()`/`require()` (batch path) and `step()`/`current()` (streaming
   path) on the same instance. Same nodes, same `add_node()`, same handle.
4. ❌ A list of indicators maintained in N places (C API table, Python register,
   Node register, QuickJS register, Codon register, docs nav, parity test
   matrix). One file: `include/flox/indicator/registry.def`.
5. ❌ **Two parallel API surfaces** for the same indicator — a "batch" one and
   a "streaming" one with different names, different handles, different
   functions. There is **one object per indicator**. It exposes both
   `compute()` (stateless, on an array) and `update()`/`value()` (stateful,
   on a stream). Same name, same handle, same docs page, same example.
   No `flox_streaming_ema_*` in C API. No `flox.streaming.EMA` in Python.
   No `EMABatch` and `EMAStreaming` classes anywhere.

---

## What "done" looks like

**One object per indicator. Two ways to use it.** Not two objects.

For a user:

```python
import flox_py as flox

ema = flox.EMA(10)               # one object

# Use it as a batch operator on an array:
out = ema.compute(prices)

# Or use it as a streaming accumulator on the same object:
for v in stream:
    ema.update(v)
    if ema.ready:
        signal(ema.value)

# compute() is stateless; update()/value/reset() share their own state.
# You don't "construct a streaming EMA" or "construct a batch EMA" —
# you construct an EMA. It can do both.
#
# Identical surface in Node, QuickJS, Codon, C++.
# Identical numerical output across all 5 surfaces, bit-for-bit.
```

`flox.ema(arr, 10)` (the snake_case top-level function) remains as a
one-shot convenience — equivalent to `flox.EMA(10).compute(arr)`. It is sugar,
not a parallel API.

For a developer adding a new indicator:

```c
// 1. Write the C++ class with compute().
// 2. Add ONE LINE here:
FLOX_INDICATOR(MyIndicator, my_indicator, SingleInput, (size_t period))
// 3. Done. C API, Python, Node, QuickJS, Codon, parity test, docs — all
//    appear automatically.
```

---

## Delivery model

This is **one PR**, on **one branch from fresh `main`**, and the success
criterion is **green CI on the final commit** — not "passes locally on the
engineer's machine".

### Branch and PR

1. Before starting: `git fetch origin && git checkout -b refactor/indicators-single-source-of-truth origin/main`.
   The branch must base off the **latest** `main` at start. If `main` advances
   during the work, rebase (don't merge) so history stays linear.
2. Push the branch to `origin`. Open one PR titled
   `refactor: single source of truth for indicators and DAG (closes #111)`.
3. The PR description references this plan and lists the acceptance criteria
   as a checkbox list, ticked one by one as each is verified on CI (not just
   locally).
4. Do not split this into multiple PRs. The whole point of the refactor is
   that all layers move together — partial merges leave the codebase in a
   half-converted state where some bindings have the new unified API and
   others still have the old duplicated one. That's worse than the starting
   point.

### CI is the gate

The job is not "done" until CI passes on the final commit of the PR. All of
these jobs from `.github/workflows/ci.yml` must be green:

- `format-check`
- `linux-gcc` (full build, all bindings, all tests, parity tests, mkdocs check)
- `linux-clang`
- `macos`
- `windows-msvc`
- `windows-clang-cl`
- `sanitizers (address)`
- `sanitizers (undefined)`
- `verify-docs-current` (added in Phase 7)
- The cross-binding parity job (added in Phase 6)

If any job is red on a push, the engineer's task is **to make it green**, not
to argue that the failure is unrelated. Real CI catches things local builds
miss (gcc-vs-clang divergence, Linux-vs-macOS-vs-Windows, libstdc++ vs
libc++, sanitizer findings). Treat every red job as a real bug.

### Iteration cadence

- Push small. Each commit should leave the build compilable and tests passing
  locally. Don't push a 50-file commit and discover on CI that 30 things broke.
- Watch CI on every push. If it's red, fix and push again.
- No `--admin` merges to bypass branch protection. No `--no-verify`. No
  `--force-with-lease` to rewrite shared history. The PR exists to be
  reviewed and audited — preserve that.
- Iterate until green. The expected number of CI rounds for a refactor this
  size is **5–15**, not 1. Plan for it.

---

## Architectural backbone: `registry.def`

Single source of truth for the list of indicators. Lives at
`include/flox/indicator/registry.def`.

```cpp
// FLOX_INDICATOR(class_name, snake_name, kind, ctor_args)
//
// kind ∈ { SingleInput, BarInput, HighLowInput, OhlcInput, PairInput, MultiOutput }
//   SingleInput   : compute(span<const double>) -> vector<double>
//   BarInput      : compute(high, low, close)
//   HighLowInput  : compute(high, low)
//   OhlcInput     : compute(open, high, low, close)
//   PairInput     : compute(x, y)
//   MultiOutput   : compute(span) -> struct (e.g. {line, signal, histogram})

FLOX_INDICATOR(EMA,               ema,                 SingleInput,  (size_t period))
FLOX_INDICATOR(SMA,               sma,                 SingleInput,  (size_t period))
FLOX_INDICATOR(RMA,               rma,                 SingleInput,  (size_t period))
FLOX_INDICATOR(RSI,               rsi,                 SingleInput,  (size_t period))
FLOX_INDICATOR(KAMA,              kama,                SingleInput,  (size_t period, size_t fast, size_t slow))
FLOX_INDICATOR(DEMA,              dema,                SingleInput,  (size_t period))
FLOX_INDICATOR(TEMA,              tema,                SingleInput,  (size_t period))
FLOX_INDICATOR(Slope,             slope,               SingleInput,  (size_t length))
FLOX_INDICATOR(Skewness,          skewness,            SingleInput,  (size_t period))
FLOX_INDICATOR(Kurtosis,          kurtosis,            SingleInput,  (size_t period))
FLOX_INDICATOR(RollingZScore,     rolling_zscore,      SingleInput,  (size_t period))
FLOX_INDICATOR(ShannonEntropy,    shannon_entropy,     SingleInput,  (size_t period, size_t bins))
FLOX_INDICATOR(AutoCorrelation,   autocorrelation,     SingleInput,  (size_t window, size_t lag))
FLOX_INDICATOR(ATR,               atr,                 BarInput,     (size_t period))
FLOX_INDICATOR(CCI,               cci,                 BarInput,     (size_t period))
FLOX_INDICATOR(Stochastic,        stochastic,          BarInput,     (size_t k_period, size_t d_period))
FLOX_INDICATOR(ParkinsonVol,      parkinson_vol,       HighLowInput, (size_t period))
FLOX_INDICATOR(RogersSatchellVol, rogers_satchell_vol, OhlcInput,    (size_t period))
FLOX_INDICATOR(Correlation,       correlation,         PairInput,    (size_t period))
FLOX_INDICATOR(MACD,              macd,                MultiOutput,  (size_t fast, size_t slow, size_t signal))
FLOX_INDICATOR(Bollinger,         bollinger,           MultiOutput,  (size_t period, double stddev))
```

Every layer that needs to know "what indicators exist" includes this file with
its own definition of `FLOX_INDICATOR`:

- C API codegen
- pybind11 registration
- N-API registration
- QuickJS function table
- Codon stubs
- Parity test matrix
- Doc generator

Add a row → it propagates everywhere. Remove a row → it disappears
everywhere. CI catches drift.

---

## Phases

### Phase 1 — Unified indicator type in C++ core

**Goal:** every indicator becomes one type that exposes BOTH `compute()`
(stateless, on an array) AND `update()`/`value()`/`ready()`/`reset()`
(stateful, on a stream). Same object. No `Streaming<T>` separate type at the
public API level.

**Implementation strategy — composition, not inheritance:**

Keep the existing math classes (`class EMA { compute(...) const; }`) untouched
and stateless — that's the canonical algorithm. Add a single generic
**facade** that pairs each math class with streaming state. The facade is
the type users see; the math class is an implementation detail.

**File:** `include/flox/indicator/streaming.h` (new — internal helper)

```cpp
namespace flox::indicator::detail {

// Internal helper. Holds an instance of the math class + accumulated history.
// Wrappers (one per Kind) below expose this through the public API surface.
template <typename Math, typename HistoryStorage>
class StatefulFacade { /* ... */ };

}  // namespace flox::indicator::detail
```

**File:** `include/flox/indicator/indicator_handle.h` (new — public)

The user-facing types live here. **One per math class, same name as the math
class.** The math class moves to `flox::indicator::math::` (private namespace),
and the public `flox::indicator::EMA` is the facade.

```cpp
namespace flox::indicator {

// Public: one type. Has both batch and streaming methods.
class EMA {
 public:
  explicit EMA(size_t period) : _math(period) {}

  // Batch — stateless, doesn't touch streaming state.
  std::vector<double> compute(std::span<const double> input) const {
    return _math.compute(input);
  }
  void compute(std::span<const double> input, std::span<double> output) const {
    _math.compute(input, output);
  }

  // Streaming — stateful. compute() and update() are independent;
  // calling compute() does NOT advance the stream.
  void   update(double v)  { _history.push_back(v); _dirty = true; }
  double value() const     { ensureFresh(); return _last; }
  bool   ready() const     { ensureFresh(); return _ready; }
  void   reset()           { _history.clear(); _last = std::nan(""); _ready = false; _dirty = false; }
  size_t count() const     { return _history.size(); }

  size_t period() const noexcept { return _math.period(); }

 private:
  void ensureFresh() const {
    if (!_dirty) return;
    if (_history.empty()) { _last = std::nan(""); _ready = false; }
    else {
      auto out = _math.compute(_history);
      _last  = out.back();
      _ready = std::isfinite(_last);
    }
    _dirty = false;
  }

  math::EMA _math;
  std::vector<double> _history;
  mutable double _last = std::nan("");
  mutable bool   _ready = false;
  mutable bool   _dirty = false;
};

}  // namespace flox::indicator
```

The body of every facade is identical mod the input arity (single value vs
high/low/close vs OHLC vs pair). Generate them via macros driven by
`registry.def`:

```cpp
// in indicator_handle.h, after the templates
#define FLOX_INDICATOR(Cls, name, Kind, Args)                             \
  using Cls = detail::Facade##Kind<math::Cls>;
#include "flox/indicator/registry.def"
#undef FLOX_INDICATOR
```

So `flox::indicator::EMA`, `flox::indicator::SMA`, `flox::indicator::ATR` are
all real types with both APIs, with **zero hand-written wrapper boilerplate**
per indicator.

**Design decision: rerun-batch-on-history (O(N) per tick).** Justified because:
- Eliminates an entire class of bugs (warmup, seeding, ring-buffer correctness).
- Parity between `obj.compute(arr)[-1]` and `for v in arr: obj.update(v); obj.value`
  is *guaranteed*, not *tested*.
- For research / UI / moderate-throughput live trading: O(N) per tick on a
  rolling window of <1000 bars is fine.
- For high-frequency: the `compute(span)` path is what you should be calling
  anyway. If profiler shows a hot streaming path on a specific indicator,
  specialize that one indicator's `update()` (private `math::EMA::Stream`
  state with O(1) advance) — **observable semantics must remain identical**,
  parity test must still pass.

**Migration of existing math classes:** rename
`flox::indicator::EMA` → `flox::indicator::math::EMA` (move into `math::`
sub-namespace). The public `flox::indicator::EMA` is now the facade. Update the
~7 internal call sites that use the math class directly.

### Phase 2 — C API: one handle per indicator, both modes on it

**Critical constraint:** the C API must NOT have `flox_indicator_ema(...)`
(batch top-level) AND `flox_streaming_ema_*(...)` (separate streaming
namespace). That's exactly the duplication this refactor exists to eliminate.

**File:** `include/flox/capi/flox_capi.h`, `src/capi/flox_capi.cpp`

```c
// Opaque handle. One per indicator instance. Has both compute and update.
typedef void* FloxIndicatorHandle;

// Lifecycle (per indicator name, generated from registry):
FloxIndicatorHandle flox_indicator_ema_create(size_t period);
void                flox_indicator_destroy(FloxIndicatorHandle h);  // generic, all kinds

// Batch path on the handle — stateless, doesn't touch streaming state:
void flox_indicator_compute(FloxIndicatorHandle h,
                            const double* input, size_t n,
                            double* output);
// (or compute_bar / compute_ohlc / compute_pair — variants on the handle,
//  not on a separate "streaming-only" handle)

// Streaming path on the SAME handle — stateful:
void   flox_indicator_update    (FloxIndicatorHandle h, double v);  // single-input kind
void   flox_indicator_update_bar(FloxIndicatorHandle h, double high, double low, double close);
void   flox_indicator_update_ohlc(FloxIndicatorHandle h, double open, double high, double low, double close);
void   flox_indicator_update_pair(FloxIndicatorHandle h, double x, double y);
double flox_indicator_value     (FloxIndicatorHandle h);
int    flox_indicator_ready     (FloxIndicatorHandle h);
void   flox_indicator_reset     (FloxIndicatorHandle h);
size_t flox_indicator_count     (FloxIndicatorHandle h);
```

There is no `flox_streaming_*` prefix. There is no separate "streaming
handle". One `flox_indicator_<name>_create()` per indicator (so the
constructor args are typed); after that, generic operations work on the
handle regardless of whether the user is calling `compute` or `update`.

Implementation in `src/capi/flox_capi.cpp` uses the same x-macro:

```cpp
#define FLOX_INDICATOR(Cls, name, Kind, Args)                       \
  FloxIndicatorHandle flox_indicator_##name##_create Args {         \
    return new flox::indicator::Cls(/* unpack Args */);             \
  }
#include "flox/indicator/registry.def"
#undef FLOX_INDICATOR
```

The generic `flox_indicator_compute / update / value / reset / destroy`
functions dispatch through the facade type — every indicator handle is a
pointer to a `flox::indicator::Cls` (the facade), and the facade has all
the methods. So one set of generic C functions works for every indicator.

**MultiOutput indicators (MACD, Bollinger):** value is a struct. Either
extend with `flox_indicator_value_named(h, "signal")` or expose typed
sub-getters. Decide during implementation; document the choice.

### Phase 3 — Delete duplicated streaming in bindings

#### Python (`python/indicator_bindings.h`)

**Delete:** all `Py*` streaming classes (lines ~75–1100). About 1000 lines.

**Replace with** registry-driven registration. Each binding exposes
`flox.<Name>` as a single class with **both** `compute()` and
`update()`/`value`/`ready`/`reset()`:

```cpp
template <typename T>
void bindSingleIndicator(py::module_& m, const char* name) {
  py::class_<T>(m, name)
      .def(py::init<size_t>(), py::arg("period"))
      // Batch on the same object — stateless query:
      .def("compute",
           [](const T& self, py::array_t<double> arr) {
             auto buf = arr.request();
             return self.compute(std::span<const double>(
                 static_cast<const double*>(buf.ptr), buf.shape[0]));
           })
      // Streaming on the same object — stateful:
      .def("update", &T::update)
      .def_property_readonly("value", &T::value)
      .def_property_readonly("ready", &T::ready)
      .def("reset",  &T::reset);
}
// ... bindBarIndicator, bindOhlcIndicator, etc.

void bindIndicators(py::module_& m) {
#define FLOX_INDICATOR(Cls, name, Kind, Args) \
  bind##Kind##Indicator<flox::indicator::Cls>(m, #Cls);
#include "flox/indicator/registry.def"
#undef FLOX_INDICATOR

  // Convenience top-level snake_case batch functions — pure sugar over
  // ClassName(args).compute(arr). Also driven by registry.
#define FLOX_INDICATOR(Cls, name, Kind, Args) \
  bindBatchSugar_##Kind<flox::indicator::Cls>(m, #name);
#include "flox/indicator/registry.def"
#undef FLOX_INDICATOR
}
```

Result: `flox.EMA(10).compute(arr)` and `flox.ema(arr, 10)` produce the same
output via the same code path. The class is the API; the function is sugar.

#### Node (`node/src/indicators.h`)

Same pattern. Delete `STREAMING_SINGLE` macros, register via N-API helper that
the registry walks.

#### QuickJS

- `quickjs/flox/indicators.js`: JS classes are facades over the unified C API.
  **The same instance has both `compute()` and `update()`/`value`/`ready`/`reset()`.**
  No `static compute()` parallel path that bypasses the instance.

  ```js
  class EMA {
    constructor(period) { this._h = __flox_indicator_ema_create(period); }
    // Batch on the instance — stateless:
    compute(arr) { return __flox_indicator_compute(this._h, arr); }
    // Streaming on the same instance — stateful:
    update(v)  { __flox_indicator_update(this._h, v); }
    get value(){ return __flox_indicator_value(this._h); }
    get ready(){ return !!__flox_indicator_ready(this._h); }
    reset()    { __flox_indicator_reset(this._h); }
    destroy()  { if (this._h) { __flox_indicator_destroy(this._h); this._h = null; } }
  }
  // Top-level sugar (one-shot batch). Pure convenience.
  function ema(arr, period) {
    const e = new EMA(period);
    try { return e.compute(arr); } finally { e.destroy(); }
  }
  ```
- `src/quickjs/js_bindings.cpp`: register the generic
  `__flox_indicator_compute / update / value / ready / reset / destroy`
  functions (one set, all kinds dispatch through the facade) plus per-name
  `__flox_indicator_<name>_create`. No `__flox_streaming_*` prefix.

#### Codon (`codon/flox/indicators.codon`)

Same — thin wrappers over the C API. Delete any local state-keeping code.

### Phase 4 — Collapse the two DAG types into one

**Problem today:** there are two public classes — `IndicatorGraph` (batch,
takes `span<const Bar>` via `setBars`) and `StreamingIndicatorGraph` (streaming
facade that holds its own `vector<Bar>` history per symbol and re-runs the
batch graph on `step()`). Two types, two doc pages, two binding wrappers per
language. That's the same "batch vs streaming as separate API surfaces"
pattern this refactor exists to kill, just one level higher.

**Goal:** **one** `IndicatorGraph` class. It owns its bars. It exposes both
the array-shaped API (`set_bars()` + `require()`) and the tick-shaped API
(`step()` + `current()`) on the same instance, sharing the same nodes, the
same cache, the same invalidation rules.

#### New unified `IndicatorGraph` (C++)

`include/flox/indicator/indicator_pipeline.h` (extended):

```cpp
class IndicatorGraph {
 public:
  using ComputeFn = std::function<std::vector<double>(IndicatorGraph&, SymbolId)>;

  // Topology — unchanged:
  void addNode(std::string name, std::vector<std::string> deps, ComputeFn fn);

  // ── Batch path: replace history wholesale, query series ──
  // Graph copies bars internally (so the user can free their span immediately).
  void setBars(SymbolId sym, std::span<const Bar> bars);

  const std::vector<double>& require(SymbolId sym, const std::string& name);
  const std::vector<double>* get    (SymbolId sym, const std::string& name) const;
  std::vector<double> close (SymbolId sym) const;
  std::vector<double> high  (SymbolId sym) const;
  std::vector<double> low   (SymbolId sym) const;
  std::vector<double> volume(SymbolId sym) const;

  // ── Streaming path: append one bar, read latest value ──
  // step() = push bar to internal history, invalidate node cache for sym.
  void step(SymbolId sym, const Bar& bar);
  // current() = last element of require(). NaN if not warm.
  double current(SymbolId sym, const std::string& name);
  size_t barCount(SymbolId sym) const;

  // Shared:
  void invalidate(SymbolId sym);
  void invalidateAll();
  void reset(SymbolId sym);   // clears history + cache for sym
  void resetAll();

 private:
  // The graph always owns its bars. set_bars() copies, step() appends.
  std::unordered_map<SymbolId, std::vector<Bar>> _bars;
  // ... existing node + cache state
};
```

**Delete** `include/flox/indicator/streaming_graph.h` and the
`StreamingIndicatorGraph` class entirely. The streaming functionality lives on
`IndicatorGraph` itself.

#### Migration of `setBars` ownership

Today `setBars(sym, span<const Bar>)` borrows the user's span. The new graph
**copies** internally — required so `step()` can extend the same internal
buffer. Trade-off: extra copy on `setBars`. Cost is bounded by historical bar
count, paid once per backtest setup, negligible vs the actual computation.

Update internal callers (~5 places) to pass `std::span` and let the graph
copy.

#### C API — one handle type

```c
// One handle. Same as the unified C++ class.
typedef void* FloxGraphHandle;

FloxGraphHandle flox_graph_create(void);
void            flox_graph_destroy(FloxGraphHandle g);

void flox_graph_add_node(FloxGraphHandle g, const char* name,
                         const char* const* deps, size_t num_deps,
                         FloxGraphNodeFn fn, void* user_data);

// Batch path:
void flox_graph_set_bars(FloxGraphHandle g, uint32_t sym,
                         const double* close, const double* high,
                         const double* low, const double* volume, size_t n);
const double* flox_graph_require(FloxGraphHandle g, uint32_t sym,
                                 const char* name, size_t* len_out);
const double* flox_graph_get    (FloxGraphHandle g, uint32_t sym,
                                 const char* name, size_t* len_out);
const double* flox_graph_close  (FloxGraphHandle g, uint32_t sym, size_t* len_out);
// ... high/low/volume

// Streaming path on the same handle:
void   flox_graph_step    (FloxGraphHandle g, uint32_t sym,
                           double open, double high, double low,
                           double close, double volume);
double flox_graph_current (FloxGraphHandle g, uint32_t sym, const char* name);
size_t flox_graph_bar_count(FloxGraphHandle g, uint32_t sym);

// Shared:
void flox_graph_invalidate    (FloxGraphHandle g, uint32_t sym);
void flox_graph_invalidate_all(FloxGraphHandle g);
void flox_graph_reset         (FloxGraphHandle g, uint32_t sym);
void flox_graph_reset_all     (FloxGraphHandle g);
```

**Delete** all `flox_streaming_graph_*` functions. The unified handle covers
both modes.

#### Bindings — one class

Python:
```python
g = flox.IndicatorGraph()                  # one type
g.add_node("ema5", [], lambda g, sym: flox.EMA(5).compute(g.close(sym)))

# Batch usage:
g.set_bars(0, close=arr, high=h, low=l, volume=v)
ema_series = g.require(0, "ema5")          # full array

# OR streaming usage on the same g:
for bar in stream:
    g.step(0, bar)
    latest = g.current(0, "ema5")          # last value
```

`StreamingIndicatorGraph` removed from `flox` namespace (the Python class
`PyStreamingIndicatorGraph` deleted from `python/graph_bindings.h`). Same in
Node, QuickJS, Codon. Old name must not exist (no alias, no deprecation —
this is pre-1.0).

#### Mixing modes

Allowed: call `set_bars(sym, history)` to seed, then call `step(sym, bar)` to
continue tick-by-tick. The graph appends to the seeded history.

Not allowed (asserts/errors): calling `set_bars` after `step` for the same
symbol replaces history (probably what the user wants, but document it
explicitly). Make the contract crisp in docs.

### Phase 5 — High-level DAG API (ergonomics)

Add **declarative helpers** alongside the existing `addNode`/`add_node` API.
Existing API stays — for cases where you need full control with custom
lambdas.

Python:
```python
g.indicator("ema5",  flox.EMA(5),  source="close")
g.indicator("ema10", flox.EMA(10), source="close")
g.derive("diff", ["ema5", "ema10"], lambda a, b: a - b)
```

Node:
```js
g.indicator("ema5",  new flox.EMA(5),  { source: "close" });
g.derive("diff", ["ema5", "ema10"], (a, b) => a.map((v, i) => v - b[i]));
```

Same pattern in QuickJS / Codon. Verbatim names where the language allows.

`g.indicator(...)` is sugar over `addNode` — internally builds the lambda that
calls `compute()` on the appropriate field.

### Phase 6 — Tests (auto-generated parity, cross-binding diff)

#### C++ parity (`tests/test_streaming_parity.cpp`)

Generic, generated from registry:

```cpp
template <typename T, typename... Args>
void checkSingleParity(Args&&... args) {
  auto input = generateRandom(50, /*seed=*/42);
  auto batchOut = T(args...).compute(input);
  Streaming<T> s(args...);
  for (auto v : input) s.update(v);
  EXPECT_NEAR(s.value(), batchOut.back(), 1e-12) << "for " << typeid(T).name();
}

#define FLOX_INDICATOR(Cls, name, Kind, Args)            \
  TEST(StreamingParity, Cls) {                            \
    check##Kind##Parity<flox::indicator::Cls>(/*defaults*/); \
  }
#include "flox/indicator/registry.def"
#undef FLOX_INDICATOR
```

#### Python tautology (`python/tests/test_parity.py`)

Walks a registry-generated list of `(class, batch_fn, default_args)` tuples and
checks `np.allclose(batch[-1], streaming_last, atol=1e-12)`. Will be a tautology
once Phase 3 lands — but still valuable as a marshalling check.

#### Cross-binding parity

Single source CSV → run through Python, Node, Codon, QuickJS → diff outputs.
Blocking CI step.

A simple driver script (Python) loads a fixed CSV, runs each binding via
subprocess, compares `np.allclose(out, ref, atol=1e-9)`.

### Phase 7 — MkDocs site for C++ and ALL bindings (mandatory)

**Not optional.** A core design goal is "user opens the docs site, sees the
same indicator with the same example in their language of choice". MkDocs
delivers that. Skipping or postponing it = the refactor is incomplete.

**Stack:**
- `mkdocs` + `mkdocs-material` theme (search, dark mode, content tabs sync,
  navigation tabs, code copy buttons).
- `mkdocstrings[python]` for Python API autodoc from docstrings on the
  built `flox_py` module.
- `pymdownx.tabbed` with `alternate_style: true` and `content.tabs.link`
  for synchronized language tabs across pages.
- `pymdownx.superfences`, `pymdownx.highlight`, `admonition`, `pymdownx.arithmatex`
  (for indicator formulas in math notation).
- Built and deployed by `.github/workflows/docs.yml` (already exists, extend it).
- Local preview: `mkdocs serve` from repo root.


#### Structure

```
docs/
  index.md                        # philosophy: single source of truth
  guides/
    indicators.md                 # one indicator type with two ways to call it
    indicator-graph.md            # one DAG type with two ways to feed it
    extending.md                  # ★ how to add a new indicator
  reference/
    cpp/
      indicators.md
      indicator-graph.md
      capi.md
    python/    { indicators.md, indicator-graph.md }
    node/      { indicators.md, indicator-graph.md }
    quickjs/   { indicators.md, indicator-graph.md }
    codon/     { indicators.md, indicator-graph.md }
```

Same structure across all binding folders so a Python user and a Node user see
the same layout.

#### Auto-gen from registry

`scripts/gen_indicator_docs.py`:
- Parses `registry.def`.
- For each indicator, writes a stub Markdown section to each binding's
  `indicators.md`:
  - Name, args, kind.
  - Batch signature (`flox.ema(arr: array, period: int) -> array`).
  - Streaming signature (`flox.EMA(period); .update(v); .value; .ready; .reset()`).
  - Minimal example.
  - Cross-link to other bindings and the C++ reference.
- Preserves hand-written content marked with `<!-- custom-start -->` /
  `<!-- custom-end -->` blocks (formulas, intuition, references).

CI gate:
```yaml
- name: Verify docs are in sync with registry.def
  run: |
    python3 scripts/gen_indicator_docs.py
    git diff --exit-code docs/ || (echo "docs out of date; run scripts/gen_indicator_docs.py" && exit 1)
```

#### `mkdocs.yml`

```yaml
site_name: Flox
site_description: Single-source-of-truth indicator framework with batch + streaming + DAG
repo_url: https://github.com/FLOX-Foundation/flox
theme:
  name: material
  features:
    - navigation.tabs
    - navigation.sections
    - content.code.copy
    - content.tabs.link        # syncs language tabs across pages
nav:
  - Home: index.md
  - Guides:
      - Indicators: guides/indicators.md
      - IndicatorGraph: guides/indicator-graph.md
      - Adding a new indicator: guides/extending.md
  - C++:
      - Indicators: reference/cpp/indicators.md
      - Streaming: reference/cpp/streaming.md
      - IndicatorGraph: reference/cpp/indicator-graph.md
      - C API: reference/cpp/capi.md
  - Python:
      - Indicators: reference/python/indicators.md
      - IndicatorGraph: reference/python/indicator-graph.md
  - Node.js:
      - Indicators: reference/node/indicators.md
      - IndicatorGraph: reference/node/indicator-graph.md
  - QuickJS:
      - Indicators: reference/quickjs/indicators.md
      - IndicatorGraph: reference/quickjs/indicator-graph.md
  - Codon:
      - Indicators: reference/codon/indicators.md
      - IndicatorGraph: reference/codon/indicator-graph.md
plugins:
  - search
  - mkdocstrings:
      handlers:
        python:
          paths: [build/python]
markdown_extensions:
  - pymdownx.superfences
  - pymdownx.tabbed:
      alternate_style: true
  - pymdownx.highlight
  - admonition
```

#### Tabbed examples — primary UX device

Every indicator page shows **the same example** in 5 tabs (Python / Node /
QuickJS / Codon / C++). With `content.tabs.link`, the user picks Python once
and sees Python on every page.

````markdown
## EMA

Exponential Moving Average. Period $N$, $\alpha = 2/(N+1)$.

=== "Python"
    ```python
    import flox_py as flox
    ema = flox.EMA(10)               # one object
    out = ema.compute(prices)        # batch on it
    for v in stream:                 # streaming on the same object
        ema.update(v)
        if ema.ready: print(ema.value)
    ```

=== "Node.js"
    ```js
    const flox = require('flox-node');
    const ema = new flox.EMA(10);
    const out = ema.compute(prices);
    for (const v of stream) {
      ema.update(v);
      if (ema.ready) console.log(ema.value);
    }
    ```

=== "QuickJS"
    ```js
    const ema = new EMA(10);
    const out = ema.compute(prices);
    ema.update(v); ema.value;
    ```

=== "Codon"
    ```python
    e = EMA(10)
    out = e.compute(prices)
    e.update(v); e.value
    ```

=== "C++"
    ```cpp
    #include <flox/indicator/indicator_handle.h>
    flox::indicator::EMA ema(10);
    auto out = ema.compute(prices);
    for (auto v : stream) {
        ema.update(v);
        if (ema.ready()) std::cout << ema.value();
    }
    ```
````

#### `docs/guides/extending.md` (the linchpin)

A how-to that is **the practical proof of the architecture**. It must work end
to end in <10 minutes for a new contributor:

```markdown
# Adding a new indicator

The framework is designed so adding a new indicator is **one class and one
line** — it appears in C++, Python, Node, QuickJS, Codon, the parity test
suite, and the docs site, all automatically.

## 1. Write the C++ class
... (file: include/flox/indicator/my_indicator.h with compute()) ...

## 2. Register it
Add ONE LINE to `include/flox/indicator/registry.def`:
    FLOX_INDICATOR(MyIndicator, my_indicator, SingleInput, (size_t period))

## 3. Build and verify
    cmake --build build
    python3 scripts/gen_indicator_docs.py
    git status            # docs/ has new entries

You now have:
- flox::indicator::Streaming<MyIndicator>     (C++)
- flox.MyIndicator(10), flox.my_indicator(arr, 10)  (Python, Node, QuickJS, Codon)
- A streaming-vs-batch parity test in CI
- Documentation pages on the site (auto-generated stub; expand with intuition/formula)

The streaming version is correct **by construction**. No separate ring buffer,
no separate alpha, no warmup logic to debug.
```

#### `.github/workflows/docs.yml`

- `verify-docs-current` job: runs gen_indicator_docs.py, fails if `git diff` is non-empty.
- `build-and-deploy` job: `mkdocs build --strict` + deploy to gh-pages on push to main.

### Phase 7.5 — Code hygiene (mandatory before every push)

This is non-negotiable for every commit in the refactor PR series.

**C / C++:**
- `clang-format -i` on every changed `.cpp` / `.h` file before staging.
- `clang-format` config lives at `.clang-format` in the repo root — use it
  unmodified.
- The `format-check` CI job (`scripts/check-format.sh`) must be green before
  push. If it's red locally, fix; do not bypass.

**Python:**
- 4-space indent, follow the existing style of `python/` and
  `python/tests/`. No `black` / `ruff` config in the repo today; match
  surrounding code.
- Run `python3 -m py_compile python/tests/*.py` to catch syntax errors before
  push.

**JavaScript (Node, QuickJS):**
- 2-space indent, single quotes, semicolons — match existing
  `node/test/test_bindings.js` and `quickjs/flox/*.js` style.

**Codon:**
- Follow `codon/flox/*.codon` existing style.

**Generated files (`scripts/gen_indicator_docs.py` output):**
- The generator must emit code that already passes the project's formatters.
  CI's `verify-docs-current` job re-runs the generator and diffs — if the
  generator emits unformatted code, that diff will be permanently dirty.

**Workflow per commit (the engineer's checklist):**
1. Make changes.
2. `clang-format -i` on changed C/C++.
3. Build: `cmake --build build`.
4. Run: `ctest`, Python/Node/QuickJS/Codon binding tests.
5. Run: `python3 scripts/gen_indicator_docs.py` if registry or doc templates
   changed; verify `git diff docs/` is reasonable.
6. Run: `mkdocs build --strict` if docs changed; zero warnings.
7. Run: `./scripts/check-format.sh` to catch what `clang-format -i` missed.
8. Commit.
9. Push only after all of the above pass locally.

If any step fails: fix the root cause, do not skip. Pre-commit hooks
(`--no-verify`) are forbidden.

### Phase 8 — Discovery API

```python
>>> flox.list_indicators()
['EMA', 'SMA', 'RMA', 'RSI', 'KAMA', 'DEMA', ...]
>>> flox.help('EMA')
EMA(period: int) — Exponential Moving Average.
  Args: period (int)
  Kind: SingleInput
  Reference: https://flox.io/reference/python/indicators.html#ema
```

Generated from registry. Same in Node / QuickJS / Codon. Tiny but useful for
notebooks and discovery.

---

## Acceptance criteria

The engineer must verify ALL of these before opening a PR:

### Architectural
1. ☐ `grep -rn "class Py.*\|PySMA\|PyEMA\|PyRSI" python/indicator_bindings.h` —
   empty.
2. ☐ `grep -rn "STREAMING_SINGLE\|STREAMING_BAR" node/src/` — empty.
3. ☐ `grep -rn "_history\|_buffer\|_alpha\|_count\|_value\b" python/indicator_bindings.h node/src/indicators.h` —
   empty (all state-keeping is in C++ core only).
4. ☐ `wc -l include/flox/indicator/registry.def` — ≥ 22 lines, single source of
   truth for the indicator set.
5. ☐ `grep -rn "flox_streaming_\|__flox_streaming_" .` — empty. There is no
   "streaming" prefix anywhere in C / C API / JS bindings. One handle, one
   class, two methods (`compute` and `update`/`value`).
6. ☐ `grep -rn "EMABatch\|EMAStreaming\|class.*Streaming.*Wrap" .` — empty.
   The user-facing type for an indicator has **one name**, not two.
7. ☐ In Python: `type(flox.EMA(10).compute(arr))` and
   `type(flox.EMA(10).update(0); flox.EMA(10).value)` both come from the same
   class. There is no `flox.streaming.EMA` namespace and no `flox.EMABatch`.
8. ☐ `grep -rn "StreamingIndicatorGraph\|streaming_graph\|flox_streaming_graph" .` —
   empty in production code. The old class is gone, not aliased.
9. ☐ The unified `IndicatorGraph` exists in C++ / C API / Python / Node /
   QuickJS / Codon as a **single type** with both `set_bars`/`require` and
   `step`/`current` methods on the same instance. Same nodes serve both
   paths. No second graph type lurking in any binding.

### Correctness
5. ☐ Existing tests pass: `ctest`, Python `test_bindings.py`, Node
   `test_bindings.js`, Codon examples, QuickJS examples.
6. ☐ New parity test (auto-gen from registry): all 22+ indicators pass with
   `atol=1e-12`.
7. ☐ Cross-binding parity job in CI: green.
8. ☐ Backward-compat: `flox.EMA(10).update(v); ema.value; flox.ema(arr, 10)`
   API unchanged.

### Extensibility
9. ☐ Add a tiny `class FooBar` indicator + one line in `registry.def` →
   without touching any binding file, `flox.FooBar(period)` works in Python,
   Node, QuickJS, Codon. Generic parity test runs on it. Doc stubs generated.
   Then revert.
10. ☐ `flox.list_indicators()` returns the full registry list.

### Ergonomics
11. ☐ `g.indicator(name, factory, source=...)` works in Python, Node, QuickJS,
    Codon (declarative, sugar over `addNode`).
12. ☐ `flox.<Indicator>(args)` and `flox.<snake>(arr, args)` patterns identical
    across all bindings.

### Docs
13. ☐ `mkdocs build --strict` — zero warnings.
14. ☐ `verify-docs-current` CI job: registry.def and docs/ in sync.
15. ☐ Indicator pages exist for every entry in `registry.def`, in every
    binding.
16. ☐ Tabbed examples render and `content.tabs.link` keeps the language choice
    sticky between pages.
17. ☐ `docs/guides/extending.md` — the engineer themselves can run the steps in
    that doc on a fresh checkout and have a new indicator working in <10 min.

### Hygiene
18. ☐ #111 closed by this PR (the umbrella issue for batch↔streaming and
    cross-binding parity).
19. ☐ No new TODOs, no commented-out blocks, no `// removed` markers from
    deleted classes.
20. ☐ `./scripts/check-format.sh` — green. Every C/C++ file
    `clang-format`-ed against the repo's `.clang-format`.
21. ☐ CI `format-check` job — green on the final commit of the PR.
22. ☐ `mkdocs build --strict` — zero warnings, zero broken links.
23. ☐ `verify-docs-current` CI job — green; `registry.def` and `docs/` in sync.
24. ☐ Local `mkdocs serve` renders every binding page (Python, Node, QuickJS,
    Codon, C++) for every indicator in the registry. No 404s.
25. ☐ No `--no-verify` / `--admin` shortcuts in the commit history of the PR.
    Every commit went through hooks and CI legitimately.
26. ☐ The PR branch is rebased on the latest `main` at the time of merge
    (not behind, no merge commits in the PR history).
27. ☐ Every CI job listed in the "Delivery model" section is **green on the
    final commit** of the PR — not green on a previous commit and red on the
    tip. The very last push must show all green.
28. ☐ The PR is one PR. Not "this PR + a follow-up". The refactor is atomic
    by design.

---

## Things explicitly OUT OF SCOPE

- **Don't** optimize streaming to O(1) per tick. The whole point is unified
  semantics. If profiler later shows a hot path, specialize that one
  indicator's `update()`, with its parity test still passing.
- **Don't** touch `compute()` math. The batch implementations are correct and
  shipped — leave them alone.
- **Don't** add new indicators. This is a refactor, not a feature PR.
- **Don't** invent a second graph type. If you find yourself wanting one,
  you're routing around the existing one — go fix the existing one instead.
- **Don't** introduce a per-binding indicator list. Registry, registry,
  registry.

---

## Order of work (suggested)

1. **Registry first.** Create `registry.def`. Even before the streaming
   wrapper. It's the spine.
2. **Streaming wrapper** in C++ core. Get C++ parity test passing for one
   indicator (e.g. EMA) end-to-end.
3. **C API codegen** from registry. Handle types, function signatures.
4. **One binding fully migrated** (Python is easiest with pybind11). Delete
   PySMA, PyEMA, etc. Replace with registry-driven registration. Run binding
   tests.
5. **Repeat for Node, QuickJS, Codon.** Each binding is independent at this
   point.
6. **Cross-binding parity** test in CI.
7. **Declarative DAG helpers** (`g.indicator`, `g.derive`).
8. **Discovery API** (`list_indicators`, `help`).
9. **Doc generator** + mkdocs site + CI gates.
10. **Final pass:** acceptance checklist top to bottom.

---

## Pointers / where things live today

- C++ indicator classes: `include/flox/indicator/{ema,sma,rsi,...}.h`
- C++ concepts: `include/flox/indicator/indicator.h` (SingleIndicator,
  BarIndicator, MultiOutputIndicator)
- C++ DAG: `include/flox/indicator/indicator_pipeline.h`,
  `include/flox/indicator/streaming_graph.h`
- C API: `include/flox/capi/flox_capi.h`, `src/capi/flox_capi.cpp`
- Python bindings: `python/indicator_bindings.h`, `python/graph_bindings.h`
- Node bindings: `node/src/indicators.h`, `node/src/graph.h`
- QuickJS: `src/quickjs/js_bindings.cpp`, `quickjs/flox/indicators.js`
- Codon: `codon/flox/indicators.codon`, `codon/flox/graph.codon`
- Existing parity test (closed PR #121): instructive but not the final form.
- Recent merged PRs setting context: #115 (targets), #116 (ADF), #117
  (AutoCorrelation), #118 (batch IndicatorGraph), #119 (StreamingIndicatorGraph).

---

*End of plan. If something here contradicts the working code, the code is
right and this doc is stale — update the doc.*
