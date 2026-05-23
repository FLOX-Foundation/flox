"""flox_overview — single-call capability summary for AI agents.

Returns a markdown narrative covering every MCP tool category and
the canonical workflows. Lives bundled (no live data); update with
each major release.

Why this exists: each tool individually carries a detailed
description in its JSONSchema registration, but there is no
top-level "what can you do?" affordance. An agent or human
connecting to the server can call this tool first and get the
overall map instead of re-deriving it from per-tool descriptions.
"""
from __future__ import annotations


_OVERVIEW = """# FLOX MCP toolkit

Call this tool first when you don't know which tool to use. It
summarises the available surface by category and lists the
canonical workflows for the most common tasks.

## Discovery (offline, bundled data — works without a running engine)

- `docs_search(query, k?)` — full-text search over how-to / reference / explanation docs.
- `lookup_symbol(name, language?)` — cross-binding symbol resolver. Finds C ABI,
  Python, Node, Codon, and QuickJS exports under any spelling
  (`Ema`, `ema`, `EMA`, `flox_indicator_ema`).
- `list_bindings(language, filter?, limit?)` — browse a binding surface.
- `examples_search(query)` / `get_example(topic, language?)` — fetch a recipe
  to copy. Recipes live under docs/examples/.
- `list_capi_functions(filter?, limit?)` — C ABI catalog.
- `list_indicators(filter?)` — indicator catalog.
- `lookup_error_code(code)` — resolve a FloxError code to its docs page.

## Building a backtest (start here for new strategies)

1. `scaffold_strategy(language, kind, name)` — emit a starter strategy class.
2. `docs_search("realistic backtest")` — venue-realistic harness via VenueStack.
3. `get_example(topic="backtest", language="python")` — copy a recipe.
4. `validate_strategy(code)` / `validate_strategy_no_lookahead(code)` — sanity
   gate before running.
5. `run_backtest(strategy_code, tape_path, ...)` — execute via the control plane.

## Live engine inspection (requires a running engine via control plane)

- `get_positions / get_pnl / get_open_orders / get_strategy_state` — read state.
- `explain_decision / explain_event` — diagnose what happened on a tick.
- `place_order / cancel_order / cancel_all` — execute via the engine.
- `flatten_positions` — force-flat across symbols.
- `set_kill_switch / get_kill_switch` — operational guard.
- `list_strategies` — what's loaded in the engine.

## Calibration + analysis

- `compute_indicator / get_indicator_values / suggest_indicator` — indicator
  data over a window.
- `whatif(...)` — counterfactual analysis (replay with overridden inputs).
- `lookahead(strategy_code, ...)` — bias detection (uses W15-T020
  estimator).
- `replay_window(start_ns, end_ns)` — re-run a slice for debugging.
- `record_data(...)` — capture market data into a `.floxlog`.

## Recent additions (W15 round 4+5)

- `VenueStack` — single-call venue-realistic backtest factory wiring
  fees + funding + liquidation + rate limits + account in one call.
  Factories: binance_um_futures, bybit_linear, okx_swap, deribit.
- `Account` — cross-margin shared state across W15 subsystems
  (LiquidationEngine, FeeSchedule). Supports cross + isolated modes.
- `LiquidationEngine.on_marks(marks, ts_ns)` — atomic multi-symbol mark
  update + walk; closes the cross-margin stale-mark footgun.
- `Account.has_stale_marks` — stale-mark guard for safe walks.
- Cross-account ADL routing — deficits route through insurance +
  ADL across every attached account.
- 4 backtest recipes under docs/examples/python_realistic_backtest.py,
  python_multi_symbol_cross_margin.py, python_funding_aware_perp_strategy.py,
  python_rate_limit_aware_market_maker.py.
- 10 new how-to pages under docs/how-to/ covering cross-margin,
  liquidation-and-adl, perpetual-funding, rate-limits, venue-downtime,
  matching-modes, self-match-prevention, order-tif-flags, iceberg-orders,
  calibrate-live-queue.

## Workflow: "make me a realistic backtest"

1. `lookup_symbol("VenueStack")` — confirm the factory exists in the user's binding.
2. `examples_search("realistic backtest")` — get the canonical recipe.
3. Copy `docs/examples/python_realistic_backtest.py` and adapt: account_id,
   equity, venue (binance_um_futures / bybit_linear / okx_swap / deribit),
   symbol set, signal logic.
4. `validate_strategy(code)` to catch obvious issues.
5. `run_backtest(...)` to execute.

For multi-symbol cross-margin accounts, use `liq.on_marks([(sym, px), ...])`
instead of `liq.on_mark(sym, px)` per symbol — atomic update prevents the
stale-mark cross-check footgun.

## Workflow: "find the right indicator"

1. `list_indicators(filter)` to browse.
2. `suggest_indicator(description)` if the user describes what they want.
3. `lookup_symbol(name)` to confirm the binding-local spelling.
4. `compute_indicator(name, tape_path, ...)` to evaluate over data.

## Workflow: "diagnose a live engine"

1. `get_strategy_state` + `get_positions` + `get_pnl` for the snapshot.
2. `get_event_log(window_ns)` for the recent event stream.
3. `explain_decision(decision_id)` or `explain_event(type, event)` for narrative.

## Things this MCP deliberately does NOT do

- Pick the user's binding for them — always ask "Python or TypeScript".
- Modify strategy code — use scaffold + validate, not autoedit.
- Reach external services — every tool reads bundled data or talks to a
  control-plane endpoint the user already runs.
"""


def flox_overview() -> str:
    return _OVERVIEW
