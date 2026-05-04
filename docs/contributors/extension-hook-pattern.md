# Extension Hook Pattern

How to add a binding-supplied callback hook to FLOX (PnLTracker, RiskManager, Executor, etc.) and wire it through every language binding.

If you're consuming a hook (writing user code in Python or Node), see the binding API docs instead. This guide is for adding the *hook itself*.

## What a hook is

A hook is a struct of C function pointers that the engine invokes at a specific point. The binding-side code wraps a user-supplied class / object behind those pointers. As of today FLOX has nine hooks:

| Hook                      | What it does                                                  | Group       |
|---------------------------|----------------------------------------------------------------|-------------|
| `PnLTracker`              | Observes every emitted signal, computes PnL                   | metrics     |
| `StorageSink`             | Persists every emitted signal                                 | storage     |
| `RiskManager`             | Pre-trade gate; can drop a signal                             | risk        |
| `KillSwitch`              | Halts trading globally                                        | risk        |
| `OrderValidator`          | Per-order validation                                          | risk        |
| `MarketDataRecorderHook`  | Receives every trade/book update fed in                       | recorder    |
| `ReplaySource`            | Custom event source for `BacktestRunner`                      | replay      |
| `Executor`                | Replaces SimulatedExecutor with a real broker / paper-trading | execution   |
| `ExecutionListener`       | Observes order lifecycle (fills / rejects / etc.)             | execution   |

Plus `set_log_callback` (single function, not a class).

## The four layers

For each hook, four things exist in lock-step:

```
1. C ABI declaration (IDL → flox_capi.h)
2. C++ implementation (src/capi/flox_capi.cpp)
3. pybind11 wrapper (python/hook_bindings.h)
4. NAPI wrapper (node/src/hooks.h)
```

If any one of those four is missing or out of sync, the [parity gate](parity-gate.md) catches it in CI.

## Layer 1: IDL declaration

In [`include/flox/capi/flox_capi_spec.hpp`](../../include/flox/capi/flox_capi_spec.hpp):

```cpp
typedef void (*FloxXxxOnEventFn)(void* user_data, const FloxSignal* sig);

typedef struct {
  FloxXxxOnEventFn on_event;
  void* user_data;
} FloxXxxCallbacks;

typedef void* FloxXxxHandle;

FLOX_EXPORT(group = "your_group_name")
FloxXxxHandle flox_xxx_create(FloxXxxCallbacks cb);
FLOX_EXPORT(group = "your_group_name")
void flox_xxx_destroy(FloxXxxHandle xxx);
```

Plus a setter on `Runner` / `LiveEngine` / `BacktestRunner`:

```cpp
FLOX_EXPORT(group = "your_group_name")
void flox_runner_set_xxx(FloxRunnerHandle runner, FloxXxxHandle xxx);
```

After editing the spec, run:

```bash
bash tools/codegen/scripts/regenerate.sh
```

This regenerates `flox_capi.h`, the Codon golden file, the Markdown reference, and the ABI snapshot.

## Layer 2: C++ implementation

In [`src/capi/flox_capi.cpp`](../../src/capi/flox_capi.cpp):

```cpp
struct FloxXxxImpl {
  FloxXxxCallbacks cb;
};

FloxXxxHandle flox_xxx_create(FloxXxxCallbacks cb) {
  return new FloxXxxImpl{cb};
}
void flox_xxx_destroy(FloxXxxHandle h) {
  delete static_cast<FloxXxxImpl*>(h);
}
```

Then **wire the hook into the place that fires it.** For a post-emission observer like PnLTracker, that means storing an atomic pointer on the signal handler and calling its `on_event` after the user callback. For a pre-trade gate like RiskManager, that means evaluating it inside `RunnerSignalHandler::onSignal` *before* the user callback. For an executor, that means routing emitted signals to it instead of `SimulatedExecutor`.

The pattern for hot-swap atomic ownership:

```cpp
// On the consumer (RunnerSignalHandler / LiveEngineImpl):
std::atomic<FloxXxxImpl*> _xxx{nullptr};

void setXxx(FloxXxxImpl* x) noexcept {
  _xxx.store(x, std::memory_order_release);
}

void onSomething(...) {
  if (auto* x = _xxx.load(std::memory_order_acquire);
      x != nullptr && x->cb.on_event != nullptr) {
    x->cb.on_event(x->cb.user_data, &payload);
  }
}
```

Lifecycle (`on_start` / `on_stop`) follows the same hot-swap pattern, balanced against engine `start()` / `stop()` so attaching mid-run fires `on_start` immediately.

## Layer 3: pybind11 wrapper

In [`python/hook_bindings.h`](../../python/hook_bindings.h):

```cpp
class PyXxx {
 public:
  virtual ~PyXxx() = default;
  virtual void on_event(const PySignal& /*sig*/) {}
};

class PyXxxTrampoline : public PyXxx {
 public:
  using PyXxx::PyXxx;
  void on_event(const PySignal& sig) override {
    PYBIND11_OVERRIDE(void, PyXxx, on_event, sig);
  }
};

// C-ABI bridge: acquires GIL, swallows Python exceptions.
inline void xxxOnEventBridge(void* ud, const FloxSignal* sig) {
  auto* py = static_cast<PyXxx*>(ud);
  invokeUnderGil([&] { py->on_event(pySignalFromC(sig)); });
}

// RAII over the C ABI handle.
class PyXxxOwner {
  std::shared_ptr<PyXxx> _delegate;
  FloxXxxHandle _handle{nullptr};
 public:
  PyXxxOwner(std::shared_ptr<PyXxx> d) : _delegate(std::move(d)) {
    FloxXxxCallbacks cb{};
    cb.on_event = xxxOnEventBridge;
    cb.user_data = _delegate.get();
    _handle = flox_xxx_create(cb);
  }
  ~PyXxxOwner() { if (_handle) flox_xxx_destroy(_handle); }
  FloxXxxHandle handle() const noexcept { return _handle; }
};
```

Then add a `set_xxx` method to `PyStrategyRunner`, `PyLiveEngine`, `PyBacktestRunner` that owns the `PyXxxOwner` and calls the C-ABI setter.

Register the class in `flox_py.cpp` (or in the registration block at the bottom of `strategy_bindings.h`):

```cpp
py::class_<PyXxx, PyXxxTrampoline, std::shared_ptr<PyXxx>>(m, "Xxx")
    .def(py::init<>())
    .def("on_event", &PyXxx::on_event, py::arg("signal"));
```

User code:

```python
class MyXxx(flox.Xxx):
    def on_event(self, sig):
        ...

runner.set_xxx(MyXxx())
```

### Important rules

1. **Acquire the GIL.** Use the `invokeUnderGil` helper. Live engine callbacks fire from C++ consumer threads where the GIL is *not* held.
2. **Never propagate Python exceptions across the C ABI boundary.** The helper catches `py::error_already_set` and prints the traceback. Letting an exception unwind through C is undefined behaviour.
3. **Hold a `shared_ptr` to the user object.** The Owner holds a non-owning C ABI handle; the user's Python class must outlive that handle. `std::shared_ptr<PyXxx>` propagates to pybind11's class registration.

## Layer 4: NAPI wrapper

In [`node/src/hooks.h`](../../node/src/hooks.h):

```cpp
struct XxxHost {
  Napi::FunctionReference on_event_fn;
  Napi::ThreadSafeFunction tsfn;
  HookMode mode;
  Napi::Env env;
  FloxXxxHandle handle{nullptr};

  XxxHost(Napi::Env env_, Napi::Object obj, HookMode m = HookMode::Sync)
      : on_event_fn(takeFn(obj, "onEvent")), mode(m), env(env_) {
    if (mode == HookMode::Threaded) {
      auto noop = Napi::Function::New(env, [](const Napi::CallbackInfo&) {});
      tsfn = Napi::ThreadSafeFunction::New(env, noop, "flox_xxx_cb", 0, 1);
    }
    FloxXxxCallbacks cb{};
    cb.on_event = &XxxHost::onEventBridge;
    cb.user_data = this;
    handle = flox_xxx_create(cb);
  }
  ~XxxHost() {
    if (handle) flox_xxx_destroy(handle);
    if (mode == HookMode::Threaded) tsfn.Release();
  }

  static void onEventBridge(void* ud, const FloxSignal* sig) {
    auto* self = static_cast<XxxHost*>(ud);
    if (self->on_event_fn.IsEmpty()) return;
    if (self->mode == HookMode::Sync) {
      // We're already on the JS thread — direct call. The result is
      // observable immediately on return from runner.onTrade etc.
      self->on_event_fn.Call({signalToJs(self->env, sig)});
      return;
    }
    // We're on a C++ consumer thread (LiveEngine). Queue via TSFN so
    // the JS handler runs at the next Node event loop tick.
    auto* copy = new FloxSignal(*sig);
    self->tsfn.NonBlockingCall(copy, [self](Napi::Env env, Napi::Function, FloxSignal* s) {
      self->on_event_fn.Call({signalToJs(env, s)});
      delete s;
    });
  }
};
```

Then add a `setXxx` method to `RunnerNode` / `BacktestRunnerNode` (`node/src/strategy.h`):

```cpp
Napi::Value setXxx(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  if (info.Length() == 0 || info[0].IsNull() || info[0].IsUndefined()) {
    _xxx_host.reset();
    flox_runner_set_xxx(_runner, nullptr);
    return env.Undefined();
  }
  _xxx_host = std::make_unique<flox_node::XxxHost>(env, info[0].As<Napi::Object>());
  flox_runner_set_xxx(_runner, _xxx_host->handle);
  return env.Undefined();
}
```

User code:

```javascript
runner.setXxx({
  onEvent(sig) { ... }
});
```

### Why two modes (Sync / Threaded)

The synchronous Runner / BacktestRunner fires hooks from the JS thread (the same thread `runner.onTrade(...)` was called from). Going through `Napi::ThreadSafeFunction::NonBlockingCall` from there would queue the JS handler for *the next event loop tick* — meaning a test that does `runner.onTrade(...)` and then immediately checks `seen.length === 1` would see `0`, because the callback hasn't run yet.

Direct `FunctionReference::Call` from the JS thread is what users actually want: the hook fires synchronously, and `await runner.something()` semantics are preserved.

For LiveEngine (`threaded: true`), callbacks fire from a C++ Disruptor consumer thread. Touching V8 from there would crash. `ThreadSafeFunction` is mandatory.

The `HookMode` flag picks the right path. Pre-trade gates (`RiskManager.allow`, `KillSwitch.check`, `OrderValidator.validate`) and `Executor.capabilities` are **Sync-only** — the engine reads the return value inline.

## Layer 5: TypeScript declaration

In [`node/index.d.ts`](../../node/index.d.ts), add an `interface` (not a `class`) for the user-supplied object:

```typescript
export interface Xxx {
  onEvent?(signal: Signal): void;
}
```

The parity gate accepts `interface` declarations as fulfilling the same role as a class.

## Tests

Mirror the existing patterns:

- pybind11: [`python/tests/test_hooks.py`](../../python/tests/test_hooks.py)
- NAPI: [`node/test/test_hooks.js`](../../node/test/test_hooks.js)

A test should: subclass / instantiate the hook → attach to runner → fire a scenario → assert the callback ran the expected number of times with the expected payload.

## Coverage gate

Open [`tools/codegen/binding_parity.yaml`](../../tools/codegen/binding_parity.yaml) and add the new group with `required` status:

```yaml
your_group_name:
  pybind11: { status: required, classes: [Xxx] }
  napi: { status: required, classes: [Xxx] }  # interface name
  codon: { status: required }
```

Run `python3 scripts/check_binding_parity.py` to verify.

## Checklist

- [ ] IDL spec updated, `regenerate.sh` ran clean
- [ ] C++ impl wired into the right firing point
- [ ] pybind11 trampoline + Owner + class registration + setter on Runner/Engine/BacktestRunner
- [ ] NAPI Host (with Sync/Threaded modes) + setter on RunnerNode/BacktestRunnerNode
- [ ] `node/index.d.ts` interface added
- [ ] Tests on both pybind11 and NAPI sides
- [ ] `binding_parity.yaml` updated to `required`
- [ ] `scripts/check_binding_parity.py` passes
- [ ] `scripts/check_dts_exports.py` passes
- [ ] `scripts/gen_pyi_stubs.py --check` passes (regenerated `.pyi`)

If all of the above pass locally, CI will be green.
