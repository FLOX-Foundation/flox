"""MCP server entry. Wires the tools into an MCP server speaking stdio."""
from __future__ import annotations

import asyncio
import logging
from typing import Any

import mcp.server.stdio
from mcp.server import Server
from mcp.types import TextContent, Tool

from .tools import (
    analytics,
    capi,
    control,
    docs_search as docs_search_tool,
    errors,
    events,
    examples,
    indicators,
    lookahead as lookahead_tool,
    lookup,
    positions,
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
                    "Return a starter FLOX strategy class that compiles "
                    "and passes `validate_strategy`. Use this as the "
                    "*first* thing you write when the user asks to "
                    "'build a new strategy' — start from this canonical "
                    "shell, then edit the indicator + signal logic, "
                    "instead of writing the FLOX bookkeeping "
                    "(constructor, hook names, signal builder) from "
                    "memory. `language` is required — FLOX is polyglot "
                    "and picking the binding for the user is wrong; "
                    "ask which language they want first. Supported: "
                    "python, node, codon, quickjs. Three kinds: "
                    "bar-driven (TA on bar close), trade-driven "
                    "(tick-by-tick), hybrid (both). The result includes "
                    "a `Next steps` section with `docs_search` queries "
                    "for the recording / backtest / layout follow-ups; "
                    "follow them in order."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "language": {
                            "type": "string",
                            "enum": ["python", "node", "codon", "quickjs"],
                            "description":
                                "Target language. Required. FLOX is "
                                "polyglot — ask the user which binding "
                                "they want before calling this tool.",
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
                    "required": ["language"],
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
                    "\n\nDispatch routing: the worker introspects the "
                    "strategy class. If `on_bar` is overridden the "
                    "dataset is dispatched as real `BarEvent`s through "
                    "`run_bars` (CSV columns: "
                    "ts,open,high,low,close,volume); otherwise the rows "
                    "are synthesised into trades for `on_trade` via "
                    "`run_csv`. A strategy that overrides neither hook "
                    "fails loudly."
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
                    "\n\nQuery syntax: plain word lists are AND-matched "
                    "(every token must appear). Wrap a phrase in double "
                    "quotes for exact match: `\"walk forward\"`. FTS5 "
                    "operators (OR, NEAR, *, parens) pass through."
                    "\n\nCanonical workflow queries (run these instead of "
                    "guessing the canonical path from training data):\n"
                    "  • User wants to record market data → "
                    "`docs_search('record tape')`. Covers live capture and "
                    "the ccxt.fetch_ohlcv historical-backfill pattern.\n"
                    "  • User asks 'how should I structure a flox project' "
                    "→ `docs_search('project layout')`.\n"
                    "  • User mentions an exchange / live data → "
                    "`docs_search('ccxt')`.\n"
                    "  • User wants to backtest a strategy on a recorded "
                    "tape → `docs_search('backtest')`.\n"
                    "  • User mentions strategy traces, signals, or "
                    "`.floxrun` → `docs_search('floxrun')`."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["query"],
                    "properties": {
                        "query": {
                            "type": "string",
                            "description":
                                "Free-text query. Plain word lists are "
                                "AND-matched. Wrap a phrase in double "
                                "quotes for exact match.",
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
            Tool(
                name="get_positions",
                description=(
                    "Read positions from a running flox engine via its "
                    "runtime state snapshot. Use this when the user asks "
                    "'what's in my positions' / 'show me the BTC position' "
                    "/ 'is strategy X long or short'. Returns a JSON object "
                    "{snapshot_age_ms, data:[{account, strategy, symbol_id, "
                    "symbol_name, qty, avg_price, unrealized_pnl}, ...]}. "
                    "Snapshot path is FLOX_RUNTIME_STATE env var or the "
                    "passed `state_path`; the user app is responsible for "
                    "writing the snapshot. Read-only — never modifies state."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "account": {
                            "type": "string",
                            "description": "Filter to one account.",
                        },
                        "strategy": {
                            "type": "string",
                            "description": "Filter to one strategy.",
                        },
                        "state_path": {
                            "type": "string",
                            "description":
                                "Override snapshot path (defaults to "
                                "FLOX_RUNTIME_STATE or "
                                "/tmp/flox-runtime-state.json).",
                        },
                    },
                },
            ),
            Tool(
                name="get_open_orders",
                description=(
                    "Read in-flight orders from the runtime state snapshot. "
                    "Use this for 'what orders are pending' / 'do I have "
                    "anything sitting on Bybit'. Optional substring filter "
                    "matches against symbol_name or strategy. Read-only."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "filter": {
                            "type": "string",
                            "description":
                                "Case-insensitive substring matched "
                                "against symbol_name or strategy.",
                        },
                        "state_path": {
                            "type": "string",
                            "description":
                                "Override snapshot path.",
                        },
                    },
                },
            ),
            Tool(
                name="get_pnl",
                description=(
                    "Read PnL totals plus per-strategy breakdown from the "
                    "runtime state snapshot. Use this for 'what's my PnL' / "
                    "'how is strategy X doing today'. Returns realized + "
                    "unrealized + fees per strategy. Read-only."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "strategy": {
                            "type": "string",
                            "description":
                                "Filter the per-strategy breakdown to "
                                "one row; total still reflects the full "
                                "snapshot.",
                        },
                        "state_path": {
                            "type": "string",
                            "description":
                                "Override snapshot path.",
                        },
                    },
                },
            ),
            Tool(
                name="get_kill_switch",
                description=(
                    "Read kill-switch state from the runtime state snapshot. "
                    "Returns {active, reason, since_ns}. Use this for 'is "
                    "trading halted' / 'why was the kill switch tripped'. "
                    "Read-only — to flip the switch use set_kill_switch."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "state_path": {
                            "type": "string",
                            "description":
                                "Override snapshot path.",
                        },
                    },
                },
            ),
            Tool(
                name="place_order",
                description=(
                    "Place an order through the user's running flox engine. "
                    "Talks HTTP to the local ControlServer the user app "
                    "embeds; reads URL + bearer token from FLOX_CONTROL_URL "
                    "and FLOX_CONTROL_TOKEN. Default dry_run=true; the "
                    "server simulates acceptance without dispatching to the "
                    "executor. live tier additionally requires an "
                    "approve_token issued out of band by the operator. Use "
                    "this for manual hedges or operator-driven order entry."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["account", "symbol", "side", "qty"],
                    "properties": {
                        "account": {
                            "type": "string",
                            "description":
                                "Account to place against. paper-prefixed "
                                "names are allowed in paper scope; live "
                                "scope is required for any other.",
                        },
                        "symbol": {"type": "integer"},
                        "side": {"type": "string", "enum": ["buy", "sell"]},
                        "qty": {"type": "number"},
                        "type": {
                            "type": "string",
                            "enum": ["market", "limit"],
                            "description": "Default market.",
                        },
                        "price": {
                            "type": "number",
                            "description":
                                "Required for limit orders; ignored for market.",
                        },
                        "reason": {
                            "type": "string",
                            "description":
                                "Free-text annotation recorded in the audit log.",
                        },
                        "dry_run": {
                            "type": "boolean",
                            "description":
                                "Default true. Set false to actually dispatch.",
                        },
                        "approve_token": {
                            "type": "string",
                            "description":
                                "Required for live scope. One-shot token "
                                "issued by ControlServer.issue_approval().",
                        },
                    },
                },
            ),
            Tool(
                name="cancel_order",
                description=(
                    "Cancel one open order by id. Talks HTTP to the local "
                    "ControlServer. Default dry_run=true. Useful for "
                    "operator-driven cleanup or panic stop."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["order_id"],
                    "properties": {
                        "order_id": {"type": "integer"},
                        "dry_run": {"type": "boolean"},
                    },
                },
            ),
            Tool(
                name="cancel_all",
                description=(
                    "Cancel every open order, optionally filtered by symbol. "
                    "Default dry_run=true. The most common 'panic stop' "
                    "primitive after set_kill_switch."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "symbol": {
                            "type": "integer",
                            "description":
                                "0 (default) cancels across all symbols.",
                        },
                        "dry_run": {"type": "boolean"},
                    },
                },
            ),
            Tool(
                name="flatten_positions",
                description=(
                    "Close every open position with opposite-side market "
                    "orders, optionally filtered by symbol. Default "
                    "dry_run=true. Use for 'close everything' operator "
                    "actions or end-of-day flatten."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "symbol": {
                            "type": "integer",
                            "description":
                                "Restrict to one symbol; omit to flatten all.",
                        },
                        "dry_run": {"type": "boolean"},
                    },
                },
            ),
            Tool(
                name="set_kill_switch",
                description=(
                    "Set the engine's kill-switch state. active=true halts "
                    "all trading; active=false resumes. Default dry_run=true; "
                    "explicit dry_run=false applies the change. Use for "
                    "emergency halt and for resuming after manual review."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["active"],
                    "properties": {
                        "active": {"type": "boolean"},
                        "reason": {
                            "type": "string",
                            "description":
                                "Free-text annotation recorded in the audit log.",
                        },
                        "dry_run": {"type": "boolean"},
                    },
                },
            ),
            # ── Live analytics (read-only) ────────────────────────
            Tool(
                name="list_strategies",
                description=(
                    "List the strategies the running flox engine knows "
                    "about, with name, status, and the symbols each one "
                    "subscribes to. Read-only. Use when the user asks "
                    "'what's running' / 'which strategies are active'."
                ),
                inputSchema={"type": "object", "properties": {}},
            ),
            Tool(
                name="get_strategy_state",
                description=(
                    "Return one strategy's current state as a JSON dict — "
                    "params, position view, last decisions. Read-only. Use "
                    "when the user asks 'what does strategy X think right "
                    "now' / 'show me state for ema-trend'."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["name"],
                    "properties": {
                        "name": {"type": "string"},
                    },
                },
            ),
            Tool(
                name="get_indicator_values",
                description=(
                    "Return the live values of indicators inside a strategy. "
                    "Optional name filter narrows to one indicator. "
                    "Read-only. Use for 'what's the EMA reading' / 'is the "
                    "RSI overbought now'."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["strategy"],
                    "properties": {
                        "strategy": {"type": "string"},
                        "name": {
                            "type": "string",
                            "description": "Optional: filter to one indicator.",
                        },
                    },
                },
            ),
            Tool(
                name="get_event_log",
                description=(
                    "Query the engine event log (signals emitted, orders "
                    "placed, fills received, risk checks). Filters AND-"
                    "compose. Read-only. Use for 'what happened in the "
                    "last 5 minutes' / 'show me all the signals from "
                    "ema-trend'. Default limit 100."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "strategy": {"type": "string"},
                        "type": {
                            "type": "string",
                            "description":
                                "Event type filter (e.g. 'signal', "
                                "'order', 'fill', 'risk_check').",
                        },
                        "from_ts_ns": {"type": "integer"},
                        "to_ts_ns": {"type": "integer"},
                        "limit": {
                            "type": "integer",
                            "description": "Max records to return. Default 100.",
                        },
                    },
                },
            ),
            Tool(
                name="explain_decision",
                description=(
                    "Walk an event's causal-parent chain back toward the "
                    "root. Returns the chain in order from the requested "
                    "event to its root. Use when the user asks 'why did "
                    "this fill happen' / 'trace the cause of order 42' — "
                    "the chain shows which signal produced the order, "
                    "which event triggered the signal, etc."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["event_id"],
                    "properties": {
                        "event_id": {"type": "string"},
                        "max_depth": {
                            "type": "integer",
                            "description":
                                "Stop walking after this many parents. Default 32.",
                        },
                    },
                },
            ),
            Tool(
                name="replay_window",
                description=(
                    "Run a sandbox replay over a time window. The engine "
                    "side decides what 'replay' means: typical setups "
                    "use the bundled tape primitives to drive a "
                    "SimulatedExecutor over a tape slice. Read-only "
                    "(sandbox-only mutations). Use for 'replay the last "
                    "hour' / 'rerun this period in sandbox'."
                ),
                inputSchema={
                    "type": "object",
                    "properties": {
                        "from_ts_ns": {"type": "integer"},
                        "to_ts_ns": {"type": "integer"},
                        "strategy": {"type": "string"},
                        "param_overrides": {
                            "type": "object",
                            "description":
                                "Optional dict of strategy params to "
                                "swap before the replay.",
                        },
                    },
                },
            ),
            Tool(
                name="whatif",
                description=(
                    "Counterfactual replay: same as replay_window plus "
                    "a strategy-name filter and required param_overrides. "
                    "Use for 'what if EMA period was 50 instead of 21' / "
                    "'show me the PnL if I had used a wider stop'."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["strategy", "param_overrides"],
                    "properties": {
                        "strategy": {"type": "string"},
                        "param_overrides": {"type": "object"},
                        "from_ts_ns": {"type": "integer"},
                        "to_ts_ns": {"type": "integer"},
                    },
                },
            ),
            Tool(
                name="validate_strategy_no_lookahead",
                description=(
                    "Static-analysis check for the most common backtest "
                    "bug: lookahead bias. Walks the strategy code's AST "
                    "and flags negative .shift(N), forward-index "
                    "arithmetic like df.iloc[i+1], open-upper slices "
                    "inside per-bar callbacks, and attribute names that "
                    "look future-dated (next_*, future_*, lookahead_*). "
                    "Heuristic, not a proof — false negatives are "
                    "possible. Run before trusting any backtest result."
                ),
                inputSchema={
                    "type": "object",
                    "required": ["code"],
                    "properties": {
                        "code": {
                            "type": "string",
                            "description":
                                "Python source of the strategy module / class.",
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
                    language=arguments.get("language"),
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
            elif name == "get_positions":
                text = positions.get_positions(
                    account=arguments.get("account"),
                    strategy=arguments.get("strategy"),
                    state_path=arguments.get("state_path"),
                )
            elif name == "get_open_orders":
                text = positions.get_open_orders(
                    filter=arguments.get("filter"),
                    state_path=arguments.get("state_path"),
                )
            elif name == "get_pnl":
                text = positions.get_pnl(
                    strategy=arguments.get("strategy"),
                    state_path=arguments.get("state_path"),
                )
            elif name == "get_kill_switch":
                text = positions.get_kill_switch(
                    state_path=arguments.get("state_path"),
                )
            elif name == "place_order":
                text = control.place_order(
                    account=arguments["account"],
                    symbol=int(arguments["symbol"]),
                    side=arguments["side"],
                    qty=float(arguments["qty"]),
                    type=arguments.get("type", "market"),
                    price=float(arguments.get("price", 0.0)),
                    reason=arguments.get("reason", ""),
                    dry_run=bool(arguments.get("dry_run", True)),
                    approve_token=arguments.get("approve_token"),
                )
            elif name == "cancel_order":
                text = control.cancel_order(
                    order_id=int(arguments["order_id"]),
                    dry_run=bool(arguments.get("dry_run", True)),
                )
            elif name == "cancel_all":
                text = control.cancel_all(
                    symbol=int(arguments.get("symbol", 0)),
                    dry_run=bool(arguments.get("dry_run", True)),
                )
            elif name == "flatten_positions":
                text = control.flatten_positions(
                    symbol=arguments.get("symbol"),
                    dry_run=bool(arguments.get("dry_run", True)),
                )
            elif name == "set_kill_switch":
                text = control.set_kill_switch(
                    active=bool(arguments["active"]),
                    reason=arguments.get("reason", ""),
                    dry_run=bool(arguments.get("dry_run", True)),
                )
            elif name == "list_strategies":
                text = analytics.list_strategies()
            elif name == "get_strategy_state":
                text = analytics.get_strategy_state(
                    name=arguments["name"],
                )
            elif name == "get_indicator_values":
                text = analytics.get_indicator_values(
                    strategy=arguments["strategy"],
                    name=arguments.get("name"),
                )
            elif name == "get_event_log":
                text = analytics.get_event_log(
                    strategy=arguments.get("strategy"),
                    type=arguments.get("type"),
                    from_ts_ns=arguments.get("from_ts_ns"),
                    to_ts_ns=arguments.get("to_ts_ns"),
                    limit=int(arguments.get("limit", 100)),
                )
            elif name == "explain_decision":
                text = analytics.explain_decision(
                    event_id=arguments["event_id"],
                    max_depth=int(arguments.get("max_depth", 32)),
                )
            elif name == "replay_window":
                text = analytics.replay_window(
                    from_ts_ns=arguments.get("from_ts_ns"),
                    to_ts_ns=arguments.get("to_ts_ns"),
                    strategy=arguments.get("strategy"),
                    param_overrides=arguments.get("param_overrides"),
                )
            elif name == "whatif":
                text = analytics.whatif(
                    strategy=arguments["strategy"],
                    param_overrides=arguments["param_overrides"],
                    from_ts_ns=arguments.get("from_ts_ns"),
                    to_ts_ns=arguments.get("to_ts_ns"),
                )
            elif name == "validate_strategy_no_lookahead":
                text = lookahead_tool.validate_strategy_no_lookahead(
                    arguments["code"],
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
