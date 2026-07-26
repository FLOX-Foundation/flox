# Run a paper engine with `flox engine sim`

`flox engine sim` boots a strategy against a recorded tape behind a control
server, so an AI agent or an operator can inspect and steer a running engine
without a live exchange. It wires up a `SimulatedExecutor`, a kill switch, a
`ControlServer` and a runtime-state writer in one command.

## Run it

```bash
flox engine sim --strategy ./my_strategy.py --tape ./tapes/bybit-btc-2026-05-07
```

The command replays the tape through the strategy, then keeps serving until
Ctrl+C so the engine can be introspected after the data ends.

| Argument | Purpose |
|----------|---------|
| `--strategy PATH` | A `.py` file holding exactly one `flox.Strategy` subclass. Two or zero subclasses is an error. |
| `--tape PATH` | A `.floxlog` directory, as written by [`flox tape record`](tape-record.md). |
| `--tape-symbol-id ID` | Which symbol id *in the tape* feeds the strategy. Required when the tape holds more than one — see below. |
| `--symbol-name NAME` | Symbol to register (default `BTCUSDT`). |
| `--exchange TAG` | Exchange tag for the registered symbol (default `sim`). |
| `--tick-size SIZE` | Tick size for the registered symbol (default `0.01`). |
| `--host` / `--port` | ControlServer bind address (default `127.0.0.1:8765`). |
| `--token TOKEN` | Explicit ControlServer token. Generated if omitted. |
| `--state-file PATH` | Runtime-state snapshot for the tier-4 MCP read tools. |
| `--threaded` | Use the Disruptor runner instead of the synchronous one. |

## Multi-symbol tapes need `--tape-symbol-id`

The command registers exactly one symbol, but a tape carries whatever ids
whoever recorded it assigned — `flox tape record` numbers them `1..N` in
encounter order, and those numbers mean nothing outside that tape.

With one symbol in the tape the mapping is unambiguous and happens
automatically. With more than one the command stops rather than guess:

```
flox engine sim: the tape holds 2 symbols ([1, 2]) but this command registers
one. Re-run with --tape-symbol-id <id> to choose which one feeds the strategy.
```

Pick the id with [`flox tape inspect`](tape-record.md), which prints the symbol
ids present, then pass it:

```bash
flox engine sim --strategy ./s.py --tape ./tapes/multi --tape-symbol-id 2
```

The trade count reported at the end counts trades *delivered to the strategy*,
not trades read from the tape, so it is the number to check when a filter is in
play.

## Drive it from an agent

The banner prints a ready-made `flox-mcp init` invocation:

```bash
flox-mcp init --engine-url http://127.0.0.1:8765 --token <generated>
```

From there the MCP tools can read positions, PnL and the event log, and flip
the kill switch — see [Inspect a running engine over MCP](mcp-runtime-inspection.md)
and [Control a running engine over MCP](mcp-control-plane.md). The kill switch gates the control server's
mutating endpoints *and* halts the strategy's own signals.

## Scope

`sim` is the only engine mode wired today, and it is paper-trading only — it
never places a real order. For live execution see
[Connect FLOX to a CCXT exchange](ccxt-adapter.md).

## Related

- [Record and replay market data](tape-record.md) — produce the tape this consumes
- [Paper trading](paper-trading.md) — the same strategy class against a live feed
- [Scaffold a project (`flox new`)](flox-new.md) — a strategy file to point `--strategy` at
