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

Future tools tracked in [W2-T004](../.notes/tracks/W2-ai-dx/T004-mcp-server.md):
`run_backtest` (sandbox), `compute_indicator`, `suggest_indicator`.

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

`scripts/sync_mcp_data.py --check` runs in CI; if a copy diverges from
its canonical source, CI fails. To update after editing the canonical
source: run the script without `--check` and commit the diff.

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
