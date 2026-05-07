# Inspect a running flox engine over MCP

The `flox-mcp` server can read positions, open orders, PnL, and the kill-switch state from a running flox engine and answer questions about them through any MCP client (Cursor, Claude Code, Cline). This is read-only inspection. Mutating ops like `place_order` and `set_kill_switch` are Phase 2 and not in this build.

## How it works

The MCP server is a child process the AI client spawns; the engine is a separate long-running process the user owns. They talk through a shared file: the user's app writes the engine state to a JSON snapshot at a known path, and the MCP tools read that snapshot on each query.

Trade-off: snapshots are point-in-time, so the agent gets state with `snapshot_age_ms` of staleness. The tools surface this in every response so the agent knows when data is fresh and when it is not.

## Snapshot path

Tools resolve the snapshot in this order:

1. The `state_path` argument passed to the tool (per-call override)
2. The `FLOX_RUNTIME_STATE` env var
3. `/tmp/flox-runtime-state.json` (default)

The user's app is responsible for writing to whichever of these paths the MCP client reads. The simplest setup is to set `FLOX_RUNTIME_STATE` in both processes' environment.

## Schema

```json
{
  "schema_version": 1,
  "captured_at_ns": 1714123456789012345,
  "kill_switch": {
    "active": false,
    "reason": null,
    "since_ns": null
  },
  "strategies": [
    {"name": "ema-trend", "status": "running", "symbols": [1]}
  ],
  "positions": [
    {
      "account": "bybit-prod",
      "strategy": "ema-trend",
      "symbol_id": 1,
      "symbol_name": "BTCUSDT",
      "qty": 0.5,
      "avg_price": 67432.10,
      "unrealized_pnl": 124.50
    }
  ],
  "open_orders": [
    {
      "order_id": "abc123",
      "account": "bybit-prod",
      "strategy": "ema-trend",
      "symbol_id": 1,
      "symbol_name": "BTCUSDT",
      "side": "BUY",
      "type": "LIMIT",
      "qty": 0.1,
      "price": 67000.0,
      "submitted_at_ns": 1714123456000000000
    }
  ],
  "pnl": {
    "by_strategy": [
      {
        "strategy": "ema-trend",
        "realized": 1234.56,
        "unrealized": 124.50,
        "fees": -12.34,
        "trades": 42
      }
    ],
    "total": {
      "realized": 1184.56,
      "unrealized": 92.50,
      "fees": -15.84
    }
  }
}
```

`schema_version` is required. Tools reject unknown versions with a clear error so a snapshot from a newer flox release does not get parsed wrong by an older MCP build.

`captured_at_ns` is required for staleness reporting. Use `time.time_ns()` at write time.

The arrays may be empty. Missing optional fields default sensibly: `kill_switch` defaults to `{active: false, reason: null, since_ns: null}` if absent.

## Writing the snapshot

flox does not ship a built-in snapshot writer in Phase 1. Every app composes its hooks differently, and the writer needs to know which hook instances to query. Here is a minimal pattern:

```python
import json
import time
from pathlib import Path

def write_snapshot(path: Path, *, kill_switch, position_tracker, pnl_tracker, strategies):
    state = {
        "schema_version": 1,
        "captured_at_ns": time.time_ns(),
        "kill_switch": {
            "active": kill_switch.is_active(),
            "reason": kill_switch.reason() or None,
            "since_ns": kill_switch.since_ns() or None,
        },
        "strategies": [
            {"name": s.name, "status": s.status, "symbols": s.symbols}
            for s in strategies
        ],
        "positions": [p.to_dict() for p in position_tracker.snapshot()],
        "open_orders": [o.to_dict() for o in position_tracker.open_orders()],
        "pnl": pnl_tracker.snapshot(),
    }
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w") as fh:
        json.dump(state, fh)
    tmp.replace(path)  # atomic on POSIX
```

Call this from a background thread or a periodic task (`asyncio.create_task` in an async runner, `threading.Timer` in a sync one). 1-second cadence works for a "what's my state" debugging UX; tighter intervals are fine if your hooks are cheap.

The atomic `tmp.replace(path)` matters: without it the MCP reader can hit a half-written file. Write to a sibling temp file, then rename.

## MCP client configuration

Add `flox-mcp` to your MCP client's server list. Pass `FLOX_RUNTIME_STATE` so it points at the same snapshot your app writes:

```json
{
  "mcpServers": {
    "flox": {
      "command": "flox-mcp",
      "env": {
        "FLOX_RUNTIME_STATE": "/var/run/flox/state.json"
      }
    }
  }
}
```

## Tools

| Tool | Purpose |
|------|---------|
| `get_positions(account?, strategy?, state_path?)` | Positions list, filterable by account or strategy. |
| `get_open_orders(filter?, state_path?)` | Open orders, with case-insensitive substring match against `symbol_name` or `strategy`. |
| `get_pnl(strategy?, state_path?)` | PnL totals plus per-strategy breakdown. |
| `get_kill_switch(state_path?)` | Returns `{active, reason, since_ns}`. |

Every tool returns `{"snapshot_age_ms": int|null, "data": ...}`. If the snapshot is missing or unreadable, the response is `{"error": "..."}`.

## What's not here yet

Phase 2 will add mutating operations (`place_order`, `cancel_order`, `cancel_all`, `flatten_positions`, `set_kill_switch`) plus a security model (scoped tokens, audit log, dry-run defaults, rate limits, out-of-band approval for live ops). Until that lands, nothing the MCP server does can change the engine's state. Flipping the kill switch or placing an order is something you still do yourself.

The snapshot model also gives stale data. A dedicated IPC transport (Unix socket or shared memory) is the natural Phase 2 follow-up for real-time inspection and for any mutating op where 1 second of staleness is unacceptable. The file-based model in Phase 1 is the cheap thing that gets us most of the value; we may regret it the first time someone wants sub-second positions.

## See also

* [MCP server tools overview](../bindings/mcp.md). The full set of `flox-mcp` tools.
* [CCXT adapter](ccxt-adapter.md). The live-feed source that typically drives the engine being inspected.
