# flox-mcp (deprecated)

`flox-mcp` is renamed to [`lrvx-mcp`](https://pypi.org/project/lrvx-mcp/).
This package is deprecated and receives no further development.

Installing it installs `lrvx-mcp`. `import flox_mcp` keeps working: it
re-exports everything `lrvx_mcp` exports, `flox_mcp.<module>` is the same
module as `lrvx_mcp.<module>`, and the `flox-mcp` command runs
`lrvx-mcp`. Each of them says it is deprecated.

Move to the new name:

```bash
pip uninstall flox-mcp
pip install lrvx-mcp
```

then point your MCP client at `lrvx-mcp` instead of `flox-mcp`
(`lrvx-mcp init` writes the client config). The migration guide is at
https://docs.lrvx.dev/how-to/migrate-from-flox/.
