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
| `lookup_symbol` | Resolve a symbol across every binding (C-API / Python / Node / Codon). Accepts any binding-local spelling (`FloxBarData`, `BarData`, `flox_indicator_ema`, `ema`). |
| `list_bindings` | Enumerate the public surface of one binding (capi / python / node / codon / quickjs). Substring filter + limit. |
| `get_example` | Canonical example code by topic (strategy / connector / indicator / event-handler / risk / backtest), optionally filtered by language. |
| `scaffold_strategy` | Starter strategy class (Python or Node, three kinds: bar-driven / trade-driven / hybrid). Python output passes `validate_strategy`; Node passes `node --check`. CI-gated against template rot. |
| `docs_search` | Top-k full-text search over the FLOX docs (FTS5 over an allowlisted corpus — never indexes private trackers, strategy notes, or `CLAUDE.md`). |

Future tools (sandboxed runtime) tracked in
[W2-T012](../.notes/tracks/W2-ai-dx/T012-mcp-follow-up-tools-run-backtest-sandbox.md):
`run_backtest`, `compute_indicator`, `suggest_indicator`.

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

The package ships read-only copies of:

- `.api/c-api.snapshot` → `flox_mcp/data/c-api.snapshot`
- `docs/errors/E_*.md` → `flox_mcp/data/errors/`
- `flox_mcp/data/ir.snapshot.json` — minimal IR (functions / structs /
  enums / typedefs / function pointers) extracted from the IDL spec
  via libclang and projected into a versioned schema.
- `flox_mcp/data/binding_manifest.json` — per-binding symbol map joined
  by IDL group, sourced from `binding_parity.yaml` plus scans of
  `flox_py/_flox_py/__init__.pyi`, `node/index.d.ts`, and the codon
  golden file.
- `flox_mcp/data/examples_index.json` — index of `docs/examples/`
  (path / language / topic / sha256).
- `flox_mcp/data/docs.fts.sqlite` — SQLite FTS5 full-text index over
  an allowlisted slice of the docs (bindings, how-to, tutorials,
  reference, explanation, errors). Internal trackers and `CLAUDE.md`
  are NEVER indexed.
- `flox_mcp/data/templates/strategy/{python,node}/{bar-driven,trade-driven,hybrid}.tmpl`
  — strategy scaffolds rendered by `scaffold_strategy`.

`scripts/sync_mcp_data.py --check` runs in CI; if any copy diverges
from its canonical source, CI fails. To update after editing a
canonical source: run the script without `--check` and commit the
diff.

The package version tracks `flox-py` and `@flox-foundation/flox` in
lockstep (one shared `set-version.sh` bump on release), so an
installed `flox-mcp x.y.z` was built off the same source tree as the
matching `flox-py` / npm package — bundled data can never describe a
surface the user does not have.

## Scope notes

- The server is **local**. No network calls. AI clients spawn it as a
  child process and talk via stdio.
- All tools are **read-only** today. `validate_strategy` parses but does
  not execute. `run_backtest` (when added) will sandbox via subprocess
  + resource limits.
- The snapshot used by `list_capi_functions` is the same `.api/c-api.snapshot`
  enforced by the codegen ABI gate; AI agents see exactly what FLOX
  ships.

## License

MIT — same as FLOX.
