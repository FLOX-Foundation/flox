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


_OVERVIEW = """# FLOX toolkit overview

Call this tool first when you don't know which tool to use or
what FLOX is capable of. FLOX is a polyglot trading framework
with a backtest core, paper and live trading paths, a replay
visualizer, and an AI-native MCP control plane. This page
covers the full surface — not just MCP tools, but the CLI and
companion modules an agent should reach for when the user wants
to do anything beyond a quick lookup.

## What FLOX is (and isn't)

FLOX is an event-driven trading framework: C++ core, first-class
bindings for Python / Node.js / Codon / QuickJS, and a stable C
ABI. Targets crypto perpetuals and spot. Backtest physics model
venue-realistic fees, funding, liquidation cascade, ADL, rate
limits, queue position, and venue downtime — surfaces that retail
frameworks (vectorbt / backtrader / freqtrade) don't cover.

Out of scope: equity / options markets, vector-matrix retail
backtest patterns (use vectorbt for those).

## CLI (`flox` after `pip install flox-py`)

- `flox new <project>` — scaffold a project. Templates: `research`
  (backtest-first), `live` (live trading), `indicator-library`
  (custom indicators).
- `flox templates` — list templates.
- `flox tape record <out>` — capture a live ccxt feed into a `.floxlog`.
- `flox tape replay <tape>` — drive a strategy off a recorded tape.
- `flox tape inspect <tape>` — header + summary.
- `flox tape diff <a> <b>` — byte-level diff between two tapes.
- `flox tape view <tape> [run]` — open the replay viewer SPA in
  a browser tab.
- `flox report <stats.json> [-o report.html]` — render backtest
  stats to an HTML report with equity curve + trades.
- `flox bundle pack / replay / validate` — reproducibility bundles
  (`.floxlog` + `.floxrun` + strategy code + config + dep hashes).
- `flox lint lookahead <strategy.py>` — static lookahead-bias
  detection.
- `flox archive {binance,bybit,okx,bitget,deribit} <symbol>` — pull
  public venue archives into floxlog tapes.

## Discovery — MCP tools (offline, no engine needed)

- `docs_search(query, k?)` — full-text search over docs.
- `lookup_symbol(name, language?)` — cross-binding symbol resolver
  (C ABI / Python / Node / Codon / QuickJS).
- `list_bindings(language, filter?, limit?)` — browse a binding.
- `examples_search(query)` / `get_example(topic, language?)` —
  fetch a recipe to copy from `docs/examples/`.
- `list_capi_functions(filter?, limit?)` — C ABI catalog.
- `list_indicators(filter?)` — indicator catalog.
- `lookup_error_code(code)` — resolve a FloxError to its docs page.

## Building a backtest

1. `scaffold_strategy(language, kind, name)` — starter Strategy class.
2. `docs_search("realistic backtest")` — venue-realistic harness via
   `flox.VenueStack`.
3. `get_example(topic="backtest")` — copy a runnable recipe.
4. `validate_strategy(code)` / `validate_strategy_no_lookahead(code)` —
   sanity gate before running.
5. `run_backtest(strategy_code, tape_path, ...)` — execute via the
   control plane.

### Realistic backtest stack

`flox.VenueStack.binance_um_futures(account_id, equity)` (also
`bybit_linear`, `okx_swap`, `deribit`) wires every venue subsystem
in one call:

- Executor + cross-margin Account + LiquidationEngine
- FeeSchedule (30d VIP ladder + per-symbol breakdown)
- FundingSchedule (8h interval or per-symbol tape)
- RateLimitPolicy (per-endpoint families: trading / market-data / account)
- VenueAvailability (partial outage / slow degradation / stale book)
- Iceberg refresh + queue model + ack-latency profile

For non-canonical venues:
`flox.assemble_custom_venue(account=..., fees=..., funding=...,
liquidation=..., rate_limits=...)` returns the same accessor surface.

## Paper trading

`flox_py.paper.PaperBroker` runs strategy against a LIVE market
data feed but routes orders to a SimulatedExecutor. Same fill
model as backtest. The bridge between backtest and live without
real capital.

Install: `pip install "flox-py[ccxt]"` for the ccxt live feed.

## Live trading via CCXT

`flox_py.ccxt.CcxtBroker` wraps `ccxt.pro`. Strategy code is
identical to backtest — `self.market_buy(...)`, `self.limit_sell(...)`
in `on_trade` / `on_bar` translate to `create_*_order` on the
exchange. Market data flows in (trades + L2 books + order
updates), orders flow out.

Workflow: backtest → paper (PaperBroker) → live (CcxtBroker) with
the same strategy class. `flox new --template=live` scaffolds a
ready-to-run live project.

## Visualization — replay viewer

`tools/replay-viewer/` is a static SPA. `flox tape view tape.floxlog
[run.floxrun]` stages the artifacts, launches a local server, opens
the browser. Six views on a shared timeline:

- Price chart with trade dots and signal bands
- Order book depth heatmap at the cursor
- Recent trades tail
- Strategy signal cards from `.floxrun`
- Orders timeline + rolling fill totals
- Equity curve

Drag, scrub, share via URL.

## Live engine inspection (running engine required)

- `get_positions / get_pnl / get_open_orders / get_strategy_state` — read.
- `explain_decision / explain_event` — narrative on what happened.
- `list_strategies` — what's loaded.

## Live engine control via MCP (`flox-mcp` Phase 2)

The MCP server can send mutating commands to a running engine under
explicit safety semantics. Three-tier authorization:

- `read` token — inspection only.
- `paper` token — orders only into accounts named `paper-*`.
- `live` token — anything; per-call out-of-band one-shot approval
  required for `place_order` on `live` scope.

Defaults are dry-run. Token-bucket rate limits per token + op
family (place_order = 1/sec sustained). Every mutating call logged
to an audit trail with bearer-token prefix + scope + redacted args
+ effects.

Mutating tools:
- `place_order / cancel_order / cancel_all / flatten_positions`
- `set_kill_switch / get_kill_switch`

## Reproducibility

- `flox bundle pack` — deterministic archive of (tape + run + code
  + config + dep hashes). `flox bundle replay` reruns it
  byte-identically. `flox bundle validate` checks the archive.
- The W15 venue stack is bit-deterministic across runs (verified
  via `test_w15_reproducibility.cpp`): identical inputs → identical
  engine state.

## Calibration + analysis

- `compute_indicator / get_indicator_values / suggest_indicator` —
  indicator data over a window.
- `whatif(...)` — counterfactual replay with overridden inputs.
- `lookahead(strategy_code, ...)` — bias detection.
- `replay_window(start_ns, end_ns)` — re-run a slice for debugging.
- `record_data(...)` — capture market data into `.floxlog`.

## Recent additions (W15 round 4 + 5)

- VenueStack single-call factory + `assemble_custom_venue` helper
- Cross-margin Account shared across W15 subsystems
- `LiquidationEngine.on_marks(...)` atomic multi-symbol update + walk
- Stale-mark guard (`Account.has_stale_marks`)
- Cross-account ADL routing across attached accounts
- Bit-deterministic reproducibility verified end-to-end
- Real-event calibration harness with analytical baselines
- 4 backtest recipes under `docs/examples/python_*`
- 10+ how-to pages on cross-margin, liquidation-and-adl,
  perpetual-funding, rate-limits, venue-downtime, matching-modes,
  self-match-prevention, order-tif-flags, iceberg-orders,
  calibrate-live-queue, realistic-backtest

## Canonical workflows

### "make me a realistic backtest"

1. `lookup_symbol("VenueStack")` — confirm the factory.
2. `examples_search("realistic backtest")` — get the recipe.
3. Adapt account_id / equity / venue / strategy signal.
4. `validate_strategy(code)` to catch issues.
5. `run_backtest(...)` to execute.

### "promote backtest to paper / live"

1. Same strategy class.
2. Paper: `from flox_py.paper import PaperBroker; broker = PaperBroker(...)`.
3. Live: `from flox_py.ccxt import CcxtBroker; broker = CcxtBroker(...)`
   with venue credentials.
4. MCP `place_order` on `paper` scope works without OOB; `live`
   scope requires per-call approval.
5. `flox new --template=live` scaffolds a complete live project.

### "inspect what just happened"

1. `flox tape view tape.floxlog run.floxrun` — visualize end-to-end.
2. Or: `get_strategy_state` + `get_positions` + `explain_decision`
   via MCP for narrative on a single tick.
3. Or: `flox report stats.json -o report.html` — static HTML
   summary for sharing.

### "find the right indicator"

1. `list_indicators(filter)` to browse.
2. `suggest_indicator(description)` if user describes intent.
3. `lookup_symbol(name)` to confirm the binding-local spelling.

## What FLOX deliberately does NOT do

- Pick the user's binding — always ask "Python or TypeScript".
- Modify strategy code — use scaffold + validate, not autoedit.
- Reach external services in MCP — every tool reads bundled data
  or talks to a control-plane endpoint the user already runs.
"""


def flox_overview() -> str:
    return _OVERVIEW
