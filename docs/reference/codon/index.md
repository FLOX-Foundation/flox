# Codon API Reference

Flox Codon bindings provide a Python-like API compiled to native code via the
[Codon compiler](https://github.com/exaloop/codon).

## Modules

| Module | Description |
|--------|-------------|
| [`flox.strategy`](strategy.md) | Strategy base class for event-driven strategies |
| [`flox.types`](types.md) | Core types: Price, Quantity, TradeData, SymbolContext |
| [`flox.indicators`](indicators.md) | Technical indicators (batch and streaming) |
| [`flox.runner`](runner.md) | Runner, BacktestRunner, Signal |
| [`flox.backtest`](backtest.md) | SimulatedExecutor, BacktestResult, BacktestStats, Engine, SignalBuilder |
| [`flox.tools`](tools.md) | Order books, position tracking, profiles, data I/O, statistics, segment ops |

The modules above have reference pages. `codon/flox/` ships 24 modules in total; the rest have no page
yet — read the source under `codon/flox/`:

| Module | Description |
|--------|-------------|
| `flox.context` | `SymbolContext` — used in the quick start below |
| `flox.bar_dispatch` | Bar dispatch recording |
| `flox.composite` | Composite order logic |
| `flox.delta_book` | Delta book encode / replay |
| `flox.engine` | Live engine handle |
| `flox.execution_algos` | Execution algorithms |
| `flox.fee_schedule` | Fee schedules |
| `flox.feed_clock` | Multi-feed clock |
| `flox.funding_schedule` | Funding schedules |
| `flox.graph` | Indicator graph |
| `flox.latency` | Latency models and distributions |
| `flox.live_queue_position` | Live queue-position tracking |
| `flox.order_group` | Order grouping |
| `flox.portfolio_risk` | Portfolio-level risk |
| `flox.rate_limit` | Rate-limit policies |
| `flox.run_trace` | Run recording and trace reading |
| `flox.tape_diff` | Tape diffing |
| `flox.targets` | Forward-looking labels (research only) |

## Quick Start

```python
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData

class MyStrategy(Strategy):
    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        if trade.price.to_double() > 100.0:
            self.emit_market_buy(self._symbols[0], 1.0)
```

Compile with:
```bash
codon build -exe -o my_strategy -lflox_capi my_strategy.codon
```

## Architecture

Codon strategies call the C API (`libflox_capi.so`) via Codon's C FFI.
Strategy callbacks are compiled to native code via Codon's C FFI.

See [Codon Bindings guide](../../bindings/codon.md) for build instructions.
