# MCP control plane

FLOX exposes a Model Context Protocol (MCP) server so AI agents can drive the engine the same way a human operator would. The point of this page is the design — why the surface looks the way it does. For the wiring guide, see [Control a running flox engine over MCP](../how-to/mcp-control-plane.md).

## The shape of the surface

The control plane has two halves:

- **Read** — strategy state, open orders, PnL, kill-switch state, decision logs, event history. Snapshot-driven; no real-time IPC needed.
- **Write** — place an order, cancel, flatten, flip the kill switch. Real-time IPC against a `ControlServer` embedded in the engine process.

Same MCP server, both halves. What the agent can actually do depends on the scope of the bearer token it presents.

## Three scopes, not one

Tokens come in three flavours: `read`, `paper`, `live`. The split is the central design choice.

- `read` cannot place orders. It can ask about everything.
- `paper` can place orders, but only into accounts whose name starts with `paper-`. Anything else gets refused at the server, not at the strategy.
- `live` can place real orders. Each one passes through an out-of-band approval step before it leaves the server.

The reason for three scopes instead of one big "is this allowed?" check is that the same AI client can usefully hold all three at different points in a session — read for exploration, paper for rehearsal, live for actual execution. A single capability flag collapses that distinction and pushes the policy into the agent, where it should not live.

## Why out-of-band approval for live

An MCP tool call is a function call. The model decides to make it the same way it decides to write a paragraph. Treating "place a real order" as just another tool call gives the model unilateral access to the operator's account, which is the wrong default no matter how good the model is.

The live scope solves this by requiring a second, separately-issued one-shot token on each `place_order` call. The operator generates that token through a different channel (a CLI prompt, a phone tap, whatever — anything that isn't the same MCP session). The server consumes it on the order, refuses replays, and writes the consumption to the audit log. The model can ask for an order; only the operator can let it through.

## Dry-run by default

Every mutating tool defaults to `dry_run=true`. The server runs the full security check, writes an audit record, and returns `accepted=true` with an empty effects list. Nothing moves. The operator inspects the audit, decides the call was legitimate, and re-runs with `dry_run=false`.

This is the same principle as the live approval flow, applied one level up: a hallucinated tool call hits the audit log, not the exchange. The cost is one extra round-trip on legitimate calls; the upside is that "the model fired the wrong tool" is a recoverable mistake.

## The audit log is the contract

Every accepted and rejected call gets a JSON line: the token id prefix (never the full token), the scope, the args with secrets redacted, the effects. In production the audit sink is mandatory.

The audit log is what the design assumes you will actually read after the fact, in the same way you read order history. If you don't have an audit habit, the rest of the safety story is paperwork.

## What this is not

The control plane is not a chat UI. It is a server that speaks MCP. The client is whichever AI agent the operator runs — Claude Desktop, a custom agent, a script with the MCP SDK. FLOX does not ship an opinionated agent and does not want to.

It is also not a magic risk layer. The kill switch, the position limits, the venue-availability hook — those are still in the engine. MCP gives the agent a way to ask the engine to do things; the engine still decides whether it will.

## See also

- [Control a running flox engine over MCP](../how-to/mcp-control-plane.md) — wiring guide
- [MCP runtime inspection](../how-to/mcp-runtime-inspection.md) — read tools
- [MCP live analytics](../how-to/mcp-live-analytics.md) — streaming reads
- [Architecture overview](architecture.md) — where the control plane sits
