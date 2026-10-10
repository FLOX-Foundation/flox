# Codon API Reference

lrvx Codon bindings provide a Python-like API compiled to native code via the
[Codon compiler](https://github.com/exaloop/codon).

## Modules

| Module | Description |
|--------|-------------|
| [`lrvx.strategy`](strategy.md) | Strategy base class for event-driven strategies |
| [`lrvx.types`](types.md) | Core types: Price, Quantity, TradeData, SymbolContext |
| [`lrvx.indicators`](indicators.md) | Technical indicators (batch and streaming) |
| [`lrvx.runner`](runner.md) | Runner, BacktestRunner, Signal |
| [`lrvx.backtest`](backtest.md) | SimulatedExecutor, BacktestResult, BacktestStats, Engine, SignalBuilder |
| [`lrvx.tools`](tools.md) | Order books, position tracking, profiles, data I/O, statistics, segment ops |

The modules above have reference pages. `codon/lrvx/` ships 24 modules in total; the rest have no page
yet — read the source under `codon/lrvx/`:

| Module | Description |
|--------|-------------|
| `lrvx.context` | `SymbolContext` — used in the quick start below |
| `lrvx.bar_dispatch` | Bar dispatch recording |
| `lrvx.composite` | Composite order logic |
| `lrvx.delta_book` | Delta book encode / replay |
| `lrvx.engine` | Live engine handle |
| `lrvx.execution_algos` | Execution algorithms |
| `lrvx.fee_schedule` | Fee schedules |
| `lrvx.feed_clock` | Multi-feed clock |
| `lrvx.funding_schedule` | Funding schedules |
| `lrvx.graph` | Indicator graph |
| `lrvx.latency` | Latency models and distributions |
| `lrvx.live_queue_position` | Live queue-position tracking |
| `lrvx.order_group` | Order grouping |
| `lrvx.portfolio_risk` | Portfolio-level risk |
| `lrvx.rate_limit` | Rate-limit policies |
| `lrvx.run_trace` | Run recording and trace reading |
| `lrvx.tape_diff` | Tape diffing |
| `lrvx.targets` | Forward-looking labels (research only) |

## Quick Start

```codon
from lrvx.strategy import Strategy
from lrvx.context import SymbolContext
from lrvx.types import TradeData

class MyStrategy(Strategy):
    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        if trade.price.to_double() > 100.0:
            self.emit_market_buy(self._symbols[0], 1.0)
```

Compile with:
```bash
codon build -exe -o my_strategy -llrvx_capi my_strategy.codon
```

## Architecture

Codon strategies call the C API (`liblrvx_capi.so`) via Codon's C FFI.
Strategy callbacks are compiled to native code via Codon's C FFI.

See [Codon Bindings guide](../../bindings/codon.md) for build instructions.
