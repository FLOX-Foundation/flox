# How-To Guides

Solve specific problems. Assumes you know the basics.

## Project setup

| Guide | Problem |
|-------|---------|
| [Scaffold a project (`flox new`)](flox-new.md) | Bootstrap research / live / indicator-library projects from a template |
| [Strategy classes](strategy-classes.md) | Structure strategy code idiomatically |
| [CCXT adapter](ccxt-adapter.md) | Connect to an exchange via the CCXT bridge |

## Backtesting and analysis

| Guide | Problem |
|-------|---------|
| [Backtesting](backtest.md) | Run strategy backtests on historical data |
| [Realistic fills](backtest-realistic-fills.md) | Slippage, queue position, partial fills |
| [Record and replay tapes](tape-record.md) | Capture live market data to `.floxlog`, replay deterministically |
| [HTML report](backtest-html-report.md) | Render an equity curve + trade table to one HTML file |
| [Interactive backtest](interactive-backtest.md) | Inspect state mid-run from a notebook |
| [Grid search](grid-search.md) | Sweep parameters over a backtest |
| [Walk-forward](walk-forward.md) | Out-of-sample validation with rolling / anchored folds |
| [Heatmap](heatmap.md) | SVG heatmap from a 2D parameter sweep |
| [White's reality check](whites-reality-check.md) | Multiple-comparison-aware significance test |
| [Log to MLflow](mlflow.md) | Send a backtest run + artifacts into an MLflow tracking server |

## Live trading

| Guide | Problem |
|-------|---------|
| [Advanced orders](advanced-orders.md) | Stop-loss, take-profit, brackets |
| [Multi-exchange trading](multi-exchange-trading.md) | Aggregate books and route across venues |
| [Inspect a running engine over MCP](mcp-runtime-inspection.md) | Read positions / PnL / kill-switch from a running engine via an AI client |
| [Paper trading](paper-trading.md) | Drive a strategy off live data but route orders to a simulator |
| [Reproducibility bundles](reproducibility-bundles.md) | Pack strategy + tape + expected output into a single tarball; replay byte-for-byte |
| [Control a running engine over MCP](mcp-control-plane.md) | Send place / cancel / flatten / kill-switch from an AI client over a token-scoped HTTP control plane |
| [Live engine analytics over MCP](mcp-live-analytics.md) | Read-only introspection of a running engine: list strategies, walk causal chains, replay-with-overrides |
| [Lookahead-bias detector](lookahead-detector.md) | Static-analysis lint that flags `.shift(-N)`, forward-index arithmetic, future-named attributes |
| [Portfolio-level risk aggregator](portfolio-risk.md) | Combine PnL and exposure across N strategies; portfolio-level kill switch on drawdown / loss / gross / concentration |
| [Backtest with latency](backtest-with-latency.md) | Add feed / order / fill latency samples to a backtest for HFT-grade fill realism |

## Indicators and aggregation

| Guide | Problem |
|-------|---------|
| [Add an indicator](add-an-indicator.md) | Wire a custom indicator into a strategy |
| [Indicator graph](indicator-graph.md) | Compose indicators into a DAG |
| [Multi-symbol indicators](multi-symbol-indicators.md) | One indicator across many symbols |
| [Bar aggregation](bar-aggregation.md) | Pre-aggregate bars for fast backtesting |
| [Custom bar policy](custom-bar-policy.md) | Hand-roll a new bar aggregation rule |
| [Volume profile](use-volume-profile.md) | Build a volume profile from trades |

## Performance and project

| Guide | Problem |
|-------|---------|
| [Optimize performance](optimize-performance.md) | Tune for minimum latency |
| [CPU affinity](cpu-affinity.md) | Pin threads to isolated cores |
| [Configuration](configuration.md) | Runtime configuration options |
| [CI configuration](ci.md) | Understand the CI pipeline |
| [Custom connector](custom-connector.md) | Add a new exchange to the connector tree |
| [Contributing](contributing.md) | Contribute to FLOX development |

## Prerequisites

These guides assume you've worked through the [tutorials](../tutorials/README.md) and understand the core FLOX concepts.
