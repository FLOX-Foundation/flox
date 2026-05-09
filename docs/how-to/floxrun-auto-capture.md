# Capture a strategy run automatically with `.floxrun`

The `.floxrun` recorder API ships standalone. By default the strategy author calls `rec.write_signal()` / `write_order_event()` / `write_fill()` by hand. This page shows how to attach the recorder once and have every event captured automatically.

## Adapter classes

`flox::run::TraceSignalHandler` wraps an existing `ISignalHandler`. Every emitted signal flows through it; the wrapper records it and then delegates to the inner handler. `flox::run::TraceExecutionListener` does the same for order lifecycle events on `IOrderExecutionListener`.

```cpp
#include "flox/run/trace_handlers.h"
#include "flox/run/trace_recorder.h"

flox::run::TraceRecorderOptions opts;
opts.strategy_id = "my-strategy";
flox::run::TraceRecorder rec("./run.floxrun", std::move(opts));

flox::run::TraceSignalHandler trace_signals(real_signal_handler, &rec);
flox::run::TraceExecutionListener trace_exec(2, real_listener, &rec);

strategy.setSignalHandler(&trace_signals);
order_exec_bus.subscribe(&trace_exec);

// Update the feed timestamp once per dispatched market-data event so
// each captured signal / order / fill carries `feed_ts_ns`:
trace_signals.setFeedTsNs(trade.exchangeTsNs);
trace_exec.setFeedTsNs(trade.exchangeTsNs);
```

The `feed_ts_ns` setter is the only piece the engine wiring still has to drive — call it once per dispatched tape event so each captured record points back at its trigger.

## What the recorder writes

| Engine event | `.floxrun` record |
|--------------|------------------|
| `Signal::market` / `limit` / `stop*` etc. | `SignalRecord` (kind 10) |
| `IOrderExecutionListener::onOrderSubmitted` | `OrderEventRecord` kind=submit |
| `onOrderAccepted` | `OrderEventRecord` kind=ack |
| `onOrderCanceled` | `OrderEventRecord` kind=cancel |
| `onOrderRejected` | `OrderEventRecord` kind=reject (carries reason) |
| `onOrderExpired` | `OrderEventRecord` kind=expire |
| `onOrderFilled` / `onOrderPartiallyFilled` | `FillRecord` (kind 12) |

A `nullptr` recorder pointer disables capture without removing the inner-handler delegation, so attaching the wrapper unconditionally during setup is safe.

## Phase status

This page covers Phase 1: the C++ adapter classes ship in `include/flox/run/trace_handlers.h` and are exercised by `tests/test_trace_handlers.cpp`. Phase 2 lifts a one-call `Runner.attach_trace_recorder(rec)` helper into every binding so polyglot strategies capture without per-language plumbing. Phase 3 adds `Runner.trace_order_event(...)` and `Runner.trace_fill(...)` so the user's executor wrapper mirrors order events + fills into the same recorder with two extra lines per callback. End-to-end auto-subscription against the executor's listener bus is a follow-up tracked separately.

## One-call attach

The `Runner` (sync mode) now exposes `attach_trace_recorder(recorder)`. Every signal the strategy emits is auto-mirrored into the recorder. Order / fill auto-capture is a follow-up — wire those through `TraceExecutionListener` against the executor's listener bus until then.

```python
# pybind11
import flox_py
rec = flox_py.TraceRecorder(path="./run.floxrun", strategy_id="trend",
                             strategy_hash="sha256:abc",
                             run_started_ns=time.time_ns())
runner = flox_py.Runner(registry, on_signal=lambda sig: None)
runner.attach_trace_recorder(rec)
runner.set_trace_feed_ts_ns(trade.exchange_ts_ns)  # call once per tape event
```

```javascript
// node
const rec = new flox.TraceRecorder({
  path: "./run.floxrun",
  strategyId: "trend",
  strategyHash: "sha256:abc",
  runStartedNs: Date.now() * 1000000,
});
const runner = new flox.Runner(registry, sig => {});
runner.attachTraceRecorder(rec);
runner.setTraceFeedTsNs(trade.exchangeTsNs);
```

Codon reaches the same C ABI symbols (`flox_runner_attach_trace_recorder`, `flox_runner_set_trace_feed_ts_ns`) directly. Pass `null` / `None` to detach.

## Mirroring order events and fills

The signal capture hook above is one half of the trace. To get a complete `.floxrun` your executor wrapper has to mirror its own callbacks into the recorder. The runner exposes two methods for this:

```python
# pybind11 — inside your executor's on_filled callback
runner.trace_fill(order_id=fill.order_id, fill_id=fill.fill_id,
                   price=fill.price, qty=fill.qty, fee=fill.fee,
                   symbol_id=fill.symbol_id, side=fill.side, liquidity=2)
```

```javascript
// node — after the executor's on_canceled fires
runner.traceOrderEvent({
  orderId, parentSignalId: 0, symbolId,
  eventKind: 1 /* Cancel */, side, orderType: 0,
  price: 0, qty: 0,
});
```

Both methods are no-ops when no recorder is attached, so wiring them unconditionally during executor setup is safe. `eventKind` matches `OrderEventKind`: 0=Submit, 1=Cancel, 2=Modify, 3=Ack, 4=Reject, 5=PartialFill, 6=Fill, 7=Expire. `liquidity`: 0=Unknown, 1=Maker, 2=Taker.

## See also

- [Record a strategy run as `.floxrun`](floxrun.md). The recorder API the adapters write into.
- [`floxrun` strategy-trace format spec](../spec/floxrun.md). The on-disk layout.
