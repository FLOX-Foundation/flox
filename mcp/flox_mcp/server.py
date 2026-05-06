"""MCP server entry. Wires the tools into an MCP server speaking stdio."""
from __future__ import annotations

import asyncio
import logging
from typing import Any

import mcp.server.stdio
from mcp.server import Server
from mcp.types import TextContent, Tool

from .tools import (
    capi,
    docs_search as docs_search_tool,
    errors,
    events,
    examples,
    indicators,
    lookup,
    runtime,
    scaffold,
    strategy,
)

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
            Tool(
                name="lookup_symbol",
                description=(
                    "Resolve a FLOX symbol across every binding (C-API, "
                    "Python, Node, Codon). Returns the local name, kind, "
                    "and signature for each binding that exports it. Use "
                    "this whenever the user names a struct, function, or "
                    "indicator and you need to know what it's called in "
                    "*their* language — never guess at the cross-language "
                    "spelling. Accepts any spelling the user knows "
                    "('FloxBarData', 'BarData', 'flox_indicator_ema', 'ema', "
                    "'Ema'). Filter to one language with the `language` arg "
                    "if the user is writing in a specific binding."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["name"],
                    "properties": {
                        "name": {
                            "type": "string",
                            "description":
                                "Symbol name in any binding's spelling. "
                                "Case-sensitive; common transformations "
                                "(Flox prefix, flox_indicator_ prefix) are "
                                "tried automatically.",
                        },
                        "language": {
                            "type": "string",
                            "description":
                                "Optional binding filter. One of: capi, "
                                "python, node, codon, quickjs.",
                        },
                    },
                },
            ),
            Tool(
                name="list_bindings",
                description=(
                    "Enumerate the public exports of one FLOX binding "
                    "surface (C-API / Python / Node / Codon / QuickJS). "
                    "Use this when the user asks 'what does the {Python|"
                    "Node|Codon} binding expose?' or when they want to "
                    "browse a binding before picking a symbol. Substring "
                    "filter is case-insensitive."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["language"],
                    "properties": {
                        "language": {
                            "type": "string",
                            "description":
                                "Binding surface. One of: capi, python, "
                                "node, codon, quickjs.",
                        },
                        "filter": {
                            "type": "string",
                            "description":
                                "Optional substring filter on symbol name.",
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
                name="get_example",
                description=(
                    "Return canonical FLOX example code for a topic, "
                    "filtered optionally by language. Use this when the "
                    "user asks 'show me how to {backtest|connect to "
                    "ccxt|wire an indicator}' BEFORE writing fresh code "
                    "from memory — the bundled examples are CI-validated, "
                    "your generated code is not. Topics: strategy, "
                    "connector, indicator, event-handler, risk, backtest. "
                    "Languages: python, node, codon, cpp."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["topic"],
                    "properties": {
                        "topic": {
                            "type": "string",
                            "description":
                                "Topic. One of: strategy, connector, "
                                "indicator, event-handler, risk, backtest.",
                        },
                        "language": {
                            "type": "string",
                            "description":
                                "Optional language filter. One of: python, "
                                "node, codon, cpp.",
                        },
                    },
                },
            ),
            Tool(
                name="scaffold_strategy",
                description=(
                    "Return a starter FLOX strategy class (Python or Node) "
                    "that compiles and passes `validate_strategy`. Use this "
                    "as the *first* thing you write when the user asks to "
                    "'build a new strategy' — start from this canonical "
                    "shell, then edit the indicator + signal logic, instead "
                    "of writing the FLOX bookkeeping (constructor, hook "
                    "names, signal builder) from memory. Three kinds: "
                    "bar-driven (TA on bar close), trade-driven (tick-by-"
                    "tick), hybrid (both). Codon / QuickJS templates are "
                    "tracked as a follow-up."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "language": {
                            "type": "string",
                            "description":
                                "Target language. python or node. "
                                "Default: python.",
                        },
                        "kind": {
                            "type": "string",
                            "description":
                                "Strategy shape. One of: bar-driven, "
                                "trade-driven, hybrid. Default: bar-driven.",
                        },
                        "name": {
                            "type": "string",
                            "description":
                                "Class name for the generated strategy. "
                                "Must be a valid identifier. Default: "
                                "MyStrategy.",
                        },
                    },
                },
            ),
            Tool(
                name="run_backtest",
                description=(
                    "Run a Python FLOX strategy against a CSV dataset "
                    "in a sandboxed subprocess (rlimits on CPU / memory "
                    "/ output size + a wall-clock timeout). Use this "
                    "when the user asks 'try this strategy on my data' "
                    "or 'does this code actually work'. Treat as MVP "
                    "sandbox: it caps resources but does NOT isolate "
                    "the filesystem or network — never aim it at "
                    "untrusted code outside a developer's own machine. "
                    "Returns the backtest stats dict as JSON plus any "
                    "stdout the strategy printed."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["strategy_code", "dataset_path"],
                    "properties": {
                        "strategy_code": {
                            "type": "string",
                            "description":
                                "Python source defining a `flox.Strategy` "
                                "subclass at module level. The worker also "
                                "accepts a top-level `STRATEGY = MyStrategy` "
                                "assignment as the entry point.",
                        },
                        "dataset_path": {
                            "type": "string",
                            "description":
                                "Absolute path to a CSV dataset on disk. "
                                "Capped at 64 MiB.",
                        },
                        "symbol": {
                            "type": "string",
                            "description":
                                "Symbol name to register before the run. "
                                "Default: BTCUSDT.",
                        },
                        "wall_timeout_s": {
                            "type": "integer",
                            "description":
                                "Wall-clock timeout in seconds. Default 60.",
                        },
                    },
                },
            ),
            Tool(
                name="compute_indicator",
                description=(
                    "Run a single FLOX indicator over a list of floats "
                    "and return the output. Use this to sanity-check "
                    "an indicator's behaviour on a small price array "
                    "BEFORE wiring it into a strategy — especially when "
                    "the indicator has window / period / smoothing "
                    "parameters whose effect isn't obvious from the "
                    "name. Input is capped at 1 MiB. Requires the "
                    "optional `flox-py` dependency."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["name", "data"],
                    "properties": {
                        "name": {
                            "type": "string",
                            "description":
                                "Indicator name in flox_py — case "
                                "either matches the class (`EMA`, "
                                "`RSI`, `Bollinger`) or the function "
                                "(`ema`, `rsi`, `vwap`).",
                        },
                        "data": {
                            "type": "array",
                            "items": {"type": "number"},
                            "description":
                                "Input series (e.g. close prices). "
                                "Up to 125k samples.",
                        },
                    },
                    "additionalProperties": True,
                },
            ),
            Tool(
                name="suggest_indicator",
                description=(
                    "Recommend FLOX indicators for an English description "
                    "of the user's intent ('trend filter', 'momentum "
                    "oscillator', 'volatility band', 'mean revert', "
                    "'regime test'). Use this when the user describes "
                    "what they want without naming an indicator — the "
                    "tool maps phrasing to a ranked shortlist of real "
                    "FLOX indicators. Pure keyword heuristic; no LLM "
                    "call. Always confirm shape with `list_indicators` "
                    "after picking one."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["description"],
                    "properties": {
                        "description": {
                            "type": "string",
                            "description":
                                "Free-text description of what kind of "
                                "indicator the user wants.",
                        },
                        "k": {
                            "type": "integer",
                            "description":
                                "How many candidates to return. Default 3.",
                        },
                    },
                },
            ),
            Tool(
                name="docs_search",
                description=(
                    "Top-k full-text search over the FLOX documentation "
                    "(how-to guides, reference pages, tutorials, error "
                    "codes, bindings overviews, explanations). Use this "
                    "to ground answers about FLOX behavior, configuration, "
                    "or APIs in the actual docs instead of relying on "
                    "training data — call it whenever the user asks 'how "
                    "do I X' / 'what does Y do' / 'where is Z documented'. "
                    "The index is built from a strict allowlist; private "
                    "tracker / strategy / author files are NEVER indexed."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["query"],
                    "properties": {
                        "query": {
                            "type": "string",
                            "description":
                                "Free-text query. Phrases like \"walk "
                                "forward\" are treated as one phrase. "
                                "Plain word lists work too; the tool "
                                "quotes them automatically.",
                        },
                        "k": {
                            "type": "integer",
                            "description":
                                "Number of results to return (1..25). "
                                "Default 5.",
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
            elif name == "lookup_symbol":
                text = lookup.lookup_symbol(
                    name=arguments["name"],
                    language=arguments.get("language"),
                )
            elif name == "list_bindings":
                text = lookup.list_bindings(
                    language=arguments["language"],
                    filter=arguments.get("filter"),
                    limit=arguments.get("limit", 50),
                )
            elif name == "get_example":
                text = examples.get_example(
                    topic=arguments["topic"],
                    language=arguments.get("language"),
                )
            elif name == "scaffold_strategy":
                text = scaffold.scaffold_strategy(
                    language=arguments.get("language", "python"),
                    kind=arguments.get("kind", "bar-driven"),
                    name=arguments.get("name", "MyStrategy"),
                )
            elif name == "docs_search":
                text = docs_search_tool.docs_search(
                    query=arguments["query"],
                    k=arguments.get("k", 5),
                )
            elif name == "run_backtest":
                text = runtime.run_backtest(
                    strategy_code=arguments["strategy_code"],
                    dataset_path=arguments["dataset_path"],
                    symbol=arguments.get("symbol", "BTCUSDT"),
                    wall_timeout_s=arguments.get(
                        "wall_timeout_s", runtime.DEFAULT_WALL_TIMEOUT_S),
                )
            elif name == "compute_indicator":
                # Strip protocol-meta keys before forwarding kwargs to
                # the indicator constructor / function.
                indicator_kwargs = {
                    k: v for k, v in arguments.items()
                    if k not in ("name", "data")
                }
                text = runtime.compute_indicator(
                    name=arguments["name"],
                    data=arguments["data"],
                    **indicator_kwargs,
                )
            elif name == "suggest_indicator":
                text = runtime.suggest_indicator(
                    description=arguments["description"],
                    k=arguments.get("k", 3),
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
