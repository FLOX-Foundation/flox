# Inspect a running flox engine over MCP

The `flox-mcp` server can read positions, open orders, PnL, and the kill-switch state from a running flox engine and answer questions about them through any MCP client (Cursor, Claude Code, Cline). The tools on this page are read-only and driven by a JSON snapshot file. Mutating ops (`place_order`, `cancel_order`, `cancel_all`, `flatten_positions`, `set_kill_switch`) ship in the same server but go through a separate, token-scoped HTTP control plane — see [Control a running engine over MCP](mcp-control-plane.md).

## How it works

The MCP server is a child process the AI client spawns; the engine is a separate long-running process the user owns. They talk through a shared file: the user's app writes the engine state to a JSON snapshot at a known path, and the MCP tools read that snapshot on each query.

Trade-off: snapshots are point-in-time, so the agent gets state with `snapshot_age_ms` of staleness. Every response carries that number.

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

flox does not ship a built-in snapshot writer. Every app composes its hooks differently, and the writer needs to know which hook instances to query. Here is a minimal pattern:

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

## Mutating tools use a different transport

The snapshot file is read-only by construction — the MCP server never writes it. The mutating tools do not read it at all; they talk HTTP to a `ControlServer` your app embeds, reading the URL and bearer token from `FLOX_CONTROL_URL` / `FLOX_CONTROL_TOKEN`.

| Tool | Purpose |
|------|---------|
| `place_order(account, symbol, side, qty, type?, price?, reason?, dry_run?, approve_token?)` | Place a market or limit order. |
| `cancel_order(order_id, dry_run?)` | Cancel one open order. |
| `cancel_all(symbol?, dry_run?)` | Cancel every open order; `symbol=0` (default) spans all symbols. |
| `flatten_positions(symbol?, dry_run?)` | Close every open position with opposite-side market orders. |
| `set_kill_switch(active, reason?, dry_run?)` | Halt or resume trading. |

All five default to `dry_run=true`; pass `dry_run=false` to dispatch. `place_order` on `live` scope additionally needs a one-shot `approve_token` from `ControlServer.issue_approval()`. Scopes, rate limits, and the audit log are covered in [Control a running engine over MCP](mcp-control-plane.md).

## Limits of the snapshot model

The snapshot model gives stale data. A dedicated IPC transport (Unix socket or shared memory) is the natural follow-up for real-time inspection, and for any workflow where 1 second of staleness is unacceptable. The file-based model is the cheap thing that gets most of the value.

## See also

* [Control a running engine over MCP](../explanation/mcp-control-plane.md). The control-plane design behind the mutating tools.
* [CCXT adapter](ccxt-adapter.md). The live-feed source that typically drives the engine being inspected.
