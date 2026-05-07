# Control a running flox engine over MCP

`flox-mcp` Phase 2 lets an AI client send mutating commands (place an order, cancel, flatten, flip the kill switch) to a running flox engine. The wiring is two-process: the user's app embeds a `ControlServer` that listens on localhost; the AI client launches `flox-mcp` as a child, and the child connects to the server with a scoped bearer token. Defaults are dry-run; live trading requires an out-of-band approval flow.

The companion read-only inspection (`get_positions`, `get_open_orders`, `get_pnl`, `get_kill_switch`) keeps using a snapshot file. Reads do not need real-time IPC. Writes do.

## Threat model and what protects what

The MCP server is a child process the AI client spawns. Anything else on the machine that can read the user's environment can also see `FLOX_CONTROL_URL` and `FLOX_CONTROL_TOKEN`. The server enforces three layered defenses:

- Scoped bearer tokens (`read`, `paper`, `live`). A `paper` token cannot place into accounts whose name does not start with `paper-`. A `live` token is required for anything else.
- Out-of-band approval for `place_order` on `live` scope. The operator generates a one-shot token through a separate channel and passes it on the request. The server consumes it and refuses replays.
- Token-bucket rate limits per token / op family. Order entry defaults to 1/sec sustained.

Audit log records every accepted and rejected mutating call with the bearer token id (a short prefix, never the full token), the scope, the args (with secrets redacted), and the resulting effects.

## Embedding the server

```python
from pathlib import Path

import flox_py as flox
from flox_py.control_server import ControlServer

registry = flox.SymbolRegistry()
sym = registry.add_symbol("paper", "BTCUSDT", tick_size=0.01)

executor = flox.SimulatedExecutor()  # or your live executor
kill_switch = MyKillSwitch()         # any object with .set(active, reason)

server = ControlServer(
    tokens={
        "read-key-xxx":  "read",
        "paper-key-xxx": "paper",
        "live-key-xxx":  "live",
    },
    executor=executor,
    kill_switch=kill_switch,
    audit_sink=Path("/var/log/flox/control-audit.jsonl"),
    host="127.0.0.1",
    port=8765,
)
server.start()

# ... your runner loop ...

server.stop()
```

The `tokens` map is the only place secrets land. Generate them with a real RNG (`secrets.token_urlsafe(32)`), keep them out of source control, and pass them through your secret manager.

The `audit_sink` is mandatory in production. The default sink writes to the `flox.control_server` logger, which is fine for development but not for compliance.

## Configuring the MCP client

Add `flox-mcp` to your MCP client's server list. Pass `FLOX_CONTROL_URL` and `FLOX_CONTROL_TOKEN`:

```json
{
  "mcpServers": {
    "flox": {
      "command": "flox-mcp",
      "env": {
        "FLOX_CONTROL_URL": "http://127.0.0.1:8765",
        "FLOX_CONTROL_TOKEN": "paper-key-xxx"
      }
    }
  }
}
```

Pick the token whose scope matches what the AI client is allowed to do. Read-only sessions should use `read`; paper-trading rehearsals should use `paper`; live operations should use `live`.

## The dry-run default

Every mutating tool defaults to `dry_run=true`. The server validates the request, runs all the security checks, writes an audit record, and returns `accepted=true` plus an empty `effects` list. No order goes out, no kill switch flips. Set `dry_run=false` to actually apply the change.

This default is deliberate. AI clients that hallucinate a tool call still hit the audit log without moving real money. The operator reviews the audit, decides the call was legitimate, and re-runs with `dry_run=false`.

## The live-tier approval flow

`place_order` on the `live` scope rejects any request without an `approve_token`. The operator issues one through a separate channel and passes it on the call:

```python
# In a CLI session the operator owns:
import flox_py
# ... boot the same ControlServer instance the MCP server talks to ...
print(server.issue_approval())
# → "f9c1e2... a one-shot token, valid for 60 seconds"
```

The operator pastes that token into the conversation, the AI client uses it on the `place_order` call, and the server consumes it. Subsequent calls with the same token fail with `403 ApprovalRequired`. The 60-second TTL is configurable.

This is the single most important defense against an AI client running away with live capital. Do not weaken it. If you find yourself wanting to skip the approval step, use the `paper` scope instead.

## Tool reference

| Tool | What it does |
|---|---|
| `place_order(account, symbol, side, qty, type, price?, reason?, dry_run, approve_token?)` | Submit a market or limit order. Live scope requires `approve_token`. |
| `cancel_order(order_id, dry_run)` | Cancel one open order by id. |
| `cancel_all(symbol?, dry_run)` | Cancel every open order; optionally restrict to one symbol. |
| `flatten_positions(symbol?, dry_run)` | Close every open position with opposite-side market orders. |
| `set_kill_switch(active, reason?, dry_run)` | Halt or resume trading at the engine level. |

Every tool returns the server's JSON response, which contains `audit_id`, `accepted`, `dry_run`, `effects`, and on rejection an `error`. The audit log records the same fields with secrets redacted.

## What's not handled yet

- HTTP transport only. Unix sockets and platform-specific IPC are deferred.
- The token map is loaded at server start and never changes. Rotation requires a restart.
- The approval store is in memory. A server restart invalidates outstanding approvals.
- Position introspection through the control plane is read-only via the snapshot file from Phase 1; the control server itself does not expose a `/positions` endpoint. If you need real-time positions for an AI agent reasoning loop, write them to the snapshot path frequently.

The first three are deliberate; the fourth pairs naturally with a future Phase 2.5 that unifies the snapshot and the socket into a single transport.

## See also

* [Inspect a running engine over MCP](mcp-runtime-inspection.md). The read-only Phase 1 that this builds on.
* [Paper trading](paper-trading.md). Use `paper` scope and a `paper-` account prefix to rehearse mutating ops without real exchange traffic.
* [Reproducibility bundles](reproducibility-bundles.md). For after-the-fact replay of what the AI agent did.
