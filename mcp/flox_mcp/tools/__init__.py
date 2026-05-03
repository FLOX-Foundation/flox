"""Each tool is a pure function: takes arguments, returns a string.

The MCP server in `flox_mcp.server` wires these into the protocol; the
tools themselves stay framework-agnostic so they can be unit-tested
without an MCP client. Test fixtures live in `mcp/tests/`.
"""
