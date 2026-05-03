"""flox_mcp — Model Context Protocol server for FLOX.

Run via the `flox-mcp` console script or `python -m flox_mcp`. The server
speaks MCP over stdio and is meant to be spawned as a child process by
Cursor / Claude Code / Cline; see README for client config.

Exposed tools:

- list_indicators       — alphabetical map of every indicator in flox_py
                          with its constructor signature and class shape.
- lookup_error_code     — given a code like "E_SYM_001", return the full
                          Markdown page from the canonical error catalog.
- list_capi_functions   — search the FLOX C-API surface (from the
                          committed ABI snapshot); returns name + signature.
- validate_strategy     — Python AST sanity-check on strategy code:
                          imports resolve, required hooks present,
                          forbidden patterns absent.
- explain_event         — describe the fields of a FLOX event struct
                          (FloxTradeData, FloxBookData, FloxBarData, ...).
"""
__version__ = "0.1.0"
