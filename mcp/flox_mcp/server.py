"""MCP server entry. Wires the tools into an MCP server speaking stdio."""
from __future__ import annotations

import asyncio
import logging
from typing import Any

import mcp.server.stdio
from mcp.server import Server
from mcp.types import TextContent, Tool

from .tools import capi, errors, events, indicators, strategy

log = logging.getLogger("flox-mcp")


def build_server() -> Server:
    """Construct the MCP server with all registered tools."""
    server = Server("flox-mcp")

    @server.list_tools()
    async def _list_tools() -> list[Tool]:
        return [
            Tool(
                name="list_indicators",
                description=(
                    "List every indicator exposed by the FLOX Python binding "
                    "with its class signature, batch function (if any), and "
                    "shape (single-input scalar, OHLC tuple, multi-output, …). "
                    "Use this when the user asks 'what indicators does FLOX "
                    "support' or before suggesting an indicator name."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "filter": {
                            "type": "string",
                            "description":
                                "Optional substring filter; case-insensitive.",
                        }
                    },
                },
            ),
            Tool(
                name="lookup_error_code",
                description=(
                    "Resolve a FLOX error code (e.g. 'E_SYM_001') to its full "
                    "Markdown documentation page — fix recipe, common causes, "
                    "diagnostics. Use whenever a FloxError is raised in user "
                    "code; never guess at a fix from the message alone."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["code"],
                    "properties": {
                        "code": {
                            "type": "string",
                            "description":
                                "Error code, e.g. 'E_SYM_001'. Case-sensitive.",
                        }
                    },
                },
            ),
            Tool(
                name="list_capi_functions",
                description=(
                    "Search the FLOX C-API surface from the committed ABI "
                    "snapshot. Returns name + return type + parameter types "
                    "for every exported `flox_*` function. Use when the user "
                    "is writing FFI code (Codon, QuickJS, Rust cgo, ctypes) "
                    "or asks 'what C symbols can I call'."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "filter": {
                            "type": "string",
                            "description":
                                "Optional substring filter on function name.",
                        },
                        "limit": {
                            "type": "integer",
                            "description":
                                "Max entries to return. Default 50.",
                        },
                    },
                },
            ),
            Tool(
                name="validate_strategy",
                description=(
                    "Static-analysis check on a Python FLOX strategy: AST "
                    "parses, expected hooks present (on_trade / on_bar), no "
                    "forbidden patterns (eval, exec, __import__ tricks). "
                    "Use this before running user-authored strategy code. "
                    "Does NOT execute the code."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["code"],
                    "properties": {
                        "code": {
                            "type": "string",
                            "description":
                                "Python source for the strategy module/class.",
                        },
                    },
                },
            ),
            Tool(
                name="explain_event",
                description=(
                    "Describe the fields of a FLOX event struct. Accepts a "
                    "type name ('FloxTradeData', 'FloxBookData', "
                    "'FloxBarData', 'FloxSymbolContext', 'FloxSignal') OR a "
                    "raw event dict; returns each field's name, type, units, "
                    "and human description. Use when the user asks 'what's "
                    "in this event'."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "type_name": {
                            "type": "string",
                            "description":
                                "Event struct name. One of: FloxTradeData, "
                                "FloxBookData, FloxBarData, FloxSymbolContext, "
                                "FloxSignal.",
                        },
                        "event": {
                            "type": "object",
                            "description":
                                "Optional event dict to introspect. If "
                                "type_name is omitted, the dict's shape is "
                                "matched against known struct shapes.",
                        },
                    },
                },
            ),
        ]

    @server.call_tool()
    async def _call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
        try:
            if name == "list_indicators":
                text = indicators.list_indicators(filter=arguments.get("filter"))
            elif name == "lookup_error_code":
                text = errors.lookup_error_code(arguments["code"])
            elif name == "list_capi_functions":
                text = capi.list_capi_functions(
                    filter=arguments.get("filter"),
                    limit=arguments.get("limit", 50),
                )
            elif name == "validate_strategy":
                text = strategy.validate_strategy(arguments["code"])
            elif name == "explain_event":
                text = events.explain_event(
                    type_name=arguments.get("type_name"),
                    event=arguments.get("event"),
                )
            else:
                text = f"unknown tool: {name}"
        except Exception as exc:  # surface real errors to the AI client
            log.exception("tool %s failed", name)
            text = f"flox-mcp error: {type(exc).__name__}: {exc}"

        return [TextContent(type="text", text=text)]

    return server


async def _serve() -> None:
    server = build_server()
    async with mcp.server.stdio.stdio_server() as (read, write):
        await server.run(read, write, server.create_initialization_options())


def main() -> None:
    """Console-script entry. Call from `flox-mcp` shim."""
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    asyncio.run(_serve())


if __name__ == "__main__":
    main()
