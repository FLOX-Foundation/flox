# Backtest recipes

End-to-end recipes for the most common backtest assembly patterns.
Each is a single Python file under `docs/examples/`, runnable as-is,
and exercises a specific composition of W15 subsystems.

| Recipe | What it shows |
| --- | --- |
| [`python_realistic_backtest.py`](../examples/python_realistic_backtest.py) | One-call venue factory; fees + funding + liquidation + rate limits wired by default |
| [`python_multi_symbol_cross_margin.py`](../examples/python_multi_symbol_cross_margin.py) | Atomic multi-symbol `on_marks` + stale-mark guard for safe cross-margin walks |
| [`python_funding_aware_perp_strategy.py`](../examples/python_funding_aware_perp_strategy.py) | Funding settlements at 8h intervals; debit/credit account equity |
| [`python_rate_limit_aware_market_maker.py`](../examples/python_rate_limit_aware_market_maker.py) | Quote loop that respects the venue's per-endpoint rate budget |

## When to reach for which

- Just starting a strategy → **realistic_backtest** as the harness;
  drop your signal logic into the position-open / on-mark loop.
- Multi-symbol cross account → **multi_symbol_cross_margin** for the
  on_marks pattern; otherwise you bake in a silent footgun where
  forgetting one symbol leaves the cross check evaluating against
  stale data.
- Holding-period > 8h or any leveraged perp → **funding_aware** so
  the funding-rate drag shows up in the backtest PnL.
- Market-making cadence (>100 actions/sec) → **rate_limit_aware**
  so the venue's ban policy doesn't surprise you in live.

## Discoverability

Each recipe is indexed by `sync_mcp_data.py` and surfaces via:

- `mcp__flox__examples_search("realistic backtest")`
- `mcp__flox__get_example("backtest")`
- `mcp__flox__docs_search("venue factory")`

AI agents writing a backtest from scratch should call `examples_search`
first; copying a recipe and adapting beats deriving the composition
from the per-subsystem API surface.
