# Live engine analytics over MCP

Phase 2 of the MCP positions work added mutating tools. The same control plane also serves a set of read-only analytics tools so an AI client can answer questions about a running flox engine without any risk of moving state. List strategies, dump current state, walk an event back to its root cause, replay a window with different params, all over the same HTTP control plane and the same scoped bearer token.

## What's available

| Tool | Purpose |
|---|---|
| `list_strategies` | Names, statuses, symbols of every running strategy. |
| `get_strategy_state(name)` | One strategy's params, position view, last decisions. |
| `get_indicator_values(strategy, name?)` | Live indicator readings inside a strategy. |
| `get_event_log(strategy?, type?, ...)` | Filterable view of recent engine events. |
| `explain_decision(event_id)` | Walk the causal-parent chain back to root. |
| `replay_window(from?, to?, ...)` | Sandbox replay over a time window. |
| `whatif(strategy, param_overrides, ...)` | Counterfactual replay with swapped params. |

All seven are read-only. They accept any scope (`read`, `paper`, `live`); there is no security distinction because nothing they do can change live state.

## Wiring up the providers

`ControlServer` accepts five optional callables for analytics. The user app passes whichever it can, and the corresponding tool answers; the rest return `{data: [], note: "..."}` rather than crashing.

```python
from flox_py.control_server import ControlServer
from flox_py.event_log import EventLog

event_log = EventLog(capacity=10_000)

server = ControlServer(
    tokens={"read-key-xxx": "read"},
    strategies=lambda: [
        {"name": s.name, "status": s.status, "symbols": s.symbols}
        for s in my_runner.strategies()
    ],
    strategy_state_provider=lambda name: my_strategies.get(name).get_state()
        if name in my_strategies else None,
    indicator_provider=lambda strat, name: [
        {"name": ind.name, "value": ind.value()}
        for ind in my_strategies[strat].indicators
        if name is None or ind.name == name
    ],
    event_log=event_log,
    replay_callback=run_sandbox_replay,
)
server.start()
```

`run_sandbox_replay(args: dict) → dict` is whatever you decide; the bundled `flox_py.bundle` helpers are a good starting point. The function takes the args the agent supplied and returns a JSON-serializable result.

Emit events from inside your hooks. The log is thread-safe; emit from any callback:

```python
sig = event_log.emit(
    "signal",
    strategy="ema-trend",
    causal_parent_id=trade_event_id,
    payload={"side": "buy", "price": 67432.10},
)
order = event_log.emit(
    "order",
    strategy="ema-trend",
    causal_parent_id=sig.event_id,
    payload={"order_id": 1234, "qty": 0.1},
)
fill = event_log.emit(
    "fill",
    strategy="ema-trend",
    causal_parent_id=order.event_id,
    payload={"price": 67432.50, "qty": 0.1},
)
```

When the agent later calls `explain_decision(fill.event_id)`, the chain walks back through `order → signal → trade`, which is what makes "why did this fill happen" answerable from one request.

## Use cases

**"What's running?"** → `list_strategies` returns an array of `{name, status, symbols}`. The agent can pivot to `get_strategy_state` for any name.

**"What does ema-trend think right now?"** → `get_strategy_state(name="ema-trend")` returns whatever your `strategy_state_provider` returns. Common payload: current params, position view, last N decisions with reasons.

**"Why did this order go out?"** → call `get_event_log` to find the order, copy its `event_id`, call `explain_decision` to get the chain. The chain shows which signal produced it, which event triggered the signal, etc.

**"What if I had used a wider stop?"** → `whatif(strategy="ema-trend", param_overrides={"stop_bps": 50})`. Your `replay_callback` receives the args, runs a sandbox replay with the swapped params, returns the diff against the live result.

## Performance

The event log is a fixed-capacity ring buffer. At the default capacity of 10k records, emit cost is negligible (a dict allocation, a uuid generation, a deque append under a lock). Tune the capacity to match your live decision rate so the agent has a useful debugging window without burning memory on stale events.

If your hooks are on a hot path, gate emits behind a flag so analytics has zero cost when no MCP client is connected:

```python
if event_log_enabled.is_set():
    event_log.emit("trade", payload={...})
```

## What this does not do

- The replay callback runs synchronously inside the HTTP server's request thread. A long replay blocks one of the threading-server worker threads. For replays longer than a few seconds, return a job id from the callback and have the agent poll a separate `get_replay_status` endpoint (a future addition).
- The event log lives in process memory. A restart loses history. Persistent event logs are a downstream concern.
- `replay_callback` is a black box from the server's perspective. Validation, sandboxing, and rate-limiting of replays are the user app's responsibility.

## See also

* [Control a running engine over MCP](mcp-control-plane.md). The Phase 2 mutating tools that share this control server.
* [Inspect a running engine over MCP](mcp-runtime-inspection.md). The Phase 1 read-only snapshot model.
* [Reproducibility bundles](reproducibility-bundles.md). Useful as the underlying primitive for `replay_callback`.
