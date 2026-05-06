# flox-mcp — Model Context Protocol server for FLOX

Gives AI agents (Cursor, Claude Code, Cline) **grounded** access to the
FLOX C-API surface, error catalog, and indicator library — so they can
write code against real signatures instead of guessing.

The server runs locally on the developer's machine; the IDE spawns it
as a child process and talks to it over stdio. There is no public
hosting; nothing leaves the machine.

## Tools

| Tool | What it does |
|---|---|
| `list_indicators` | Every indicator in `flox_py` with class signature, batch fn (if any), and shape. Filter by substring. |
| `lookup_error_code` | `E_SYM_001` → full Markdown page with fix recipe, common causes, diagnostics. |
| `list_capi_functions` | Search the FLOX C-API surface from the committed ABI snapshot. Returns `name + return type + parameter types`. |
| `validate_strategy` | Static-analysis check on Python strategy code: AST parses, expected hooks present, no `eval`/`exec`. |
| `explain_event` | Describe the fields of a FLOX event struct (`FloxTradeData`, `FloxBookData`, `FloxBarData`, `FloxSymbolContext`, `FloxSignal`). Accepts a struct name or a raw event dict. |
| `lookup_symbol` | Take any binding-local spelling (`FloxBarData`, `BarData`, `flox_indicator_ema`, `ema`) and return what the symbol is called in C-API, Python, Node, and Codon. |
| `list_bindings` | Enumerate the exports of one binding (capi, python, node, codon, quickjs). Substring filter and limit. |
| `get_example` | Code from `docs/examples/` matching a topic (strategy, connector, indicator, event-handler, risk, backtest), optionally filtered by language. |
| `scaffold_strategy` | Starter strategy class for Python or Node. Three kinds: bar-driven, trade-driven, hybrid. The Python output goes through `ast.parse` + `validate_strategy`; the Node output goes through `node --check`. CI fails if either breaks, so templates can't quietly rot. |
| `docs_search` | Top-k FTS5 search over the docs. The index is built from an allowlist of roots; private trackers and `CLAUDE.md` are not in that allowlist. |
| `run_backtest` | Run a Python strategy against a CSV dataset in a sandboxed subprocess. Caps CPU, memory, and output size; wall-clock timeout. **MVP sandbox** — caps resources but does not isolate filesystem or network. Treat the same way as any untrusted Python; use nsjail / firejail / Docker for production. |
| `compute_indicator` | Run one FLOX indicator over a list of floats. Accepts class-form (`EMA`) or function-form (`ema`) names and forwards extra kwargs to the constructor. Input capped at 1 MiB. Needs `flox-py` installed (`pip install "flox-mcp[flox]"`). |
| `suggest_indicator` | Map an English description ('trend filter', 'momentum oscillator', 'volatility band', 'mean revert', 'regime test') to a ranked shortlist of FLOX indicators. Pure keyword heuristic — no LLM call. Always confirm the chosen indicator with `list_indicators` before using it. |

## Install

```bash
pip install flox-mcp
# or with the optional binding for indicator introspection:
pip install "flox-mcp[flox]"
```

## Configure your AI client

### Claude Code

Edit `~/.claude.json` (or per-project `.claude/settings.json`):

```json
{
  "mcpServers": {
    "flox": {
      "command": "flox-mcp"
    }
  }
}
```

### Cursor

Settings → Features → MCP Servers → Add:

```json
{
  "flox": { "command": "flox-mcp" }
}
```

### Cline (VS Code)

Cline's MCP settings panel; same shape.

## Develop

```bash
cd mcp
pip install -e ".[dev]"

# Run the server manually (stdio):
flox-mcp

# Run unit tests:
pytest tests/

# Sync bundled data from canonical sources (.api/, docs/errors/):
python ../scripts/sync_mcp_data.py
```

## Bundled data

Read-only copies in the wheel:

- `flox_mcp/data/c-api.snapshot` — copy of `.api/c-api.snapshot`.
- `flox_mcp/data/errors/E_*.md` — copies of `docs/errors/E_*.md`.
- `flox_mcp/data/ir.snapshot.json` — IR (functions, structs, enums,
  typedefs, function pointers) pulled from the IDL spec via libclang.
  Versioned schema.
- `flox_mcp/data/binding_manifest.json` — per-binding symbol map keyed
  by IDL group. Built from `binding_parity.yaml` plus scans of
  `flox_py/_flox_py/__init__.pyi`, `node/index.d.ts`, and the codon
  golden file.
- `flox_mcp/data/examples_index.json` — index of `docs/examples/`
  (path, language, topic, sha256).
- `flox_mcp/data/docs.fts.sqlite` — SQLite FTS5 index over six doc
  roots: `bindings`, `how-to`, `tutorials`, `reference`, `explanation`,
  `errors`. Anything outside that list is skipped at index build time.
  Private trackers and `CLAUDE.md` are not in the allowlist.
- `flox_mcp/data/templates/strategy/{python,node}/{bar,trade,hybrid}-driven.tmpl`
  — strategy scaffolds rendered by `scaffold_strategy`.

`scripts/sync_mcp_data.py --check` runs in CI. Any drift between a
bundled copy and its canonical source fails the build. To update
after editing a canonical source: run the script without `--check`
and commit the diff.

The `flox-mcp` package version is bumped in lockstep with `flox-py`
and `@flox-foundation/flox` by `scripts/set-version.sh`. An installed
`flox-mcp x.y.z` was built from the same source tree as the matching
flox-py / npm release, so what the agent sees lines up with what the
developer has installed.

## Scope notes

- The server is **local**. No network calls. AI clients spawn it as a
  child process and talk via stdio.
- Most tools are read-only data lookups. The two that execute code —
  `compute_indicator` and `run_backtest` — run in-process / in a
  resource-limited subprocess respectively. `run_backtest` is an MVP
  sandbox: it caps CPU, memory, and output size, but does NOT isolate
  filesystem or network. Wrap with nsjail / firejail / Docker for any
  deployment that takes untrusted input.
- The snapshot used by `list_capi_functions` is the same
  `.api/c-api.snapshot` enforced by the codegen ABI gate; the IR
  snapshot used by `lookup_symbol` / `list_bindings` is regenerated
  from the same IDL spec on every release. So the agent's view of
  the surface tracks what FLOX actually ships.

## License

MIT — same as FLOX.
