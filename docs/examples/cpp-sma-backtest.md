# C++ — SMA backtest

A C++ backtest of an SMA(10/30) crossover. Same model as the [Python example](python-backtest-vs-live.md) — one Strategy subclass replayed through `BacktestRunner` against a CSV — but written against the C++ API directly.

!!! warning "Illustrative only — not wired into the build"
    There is no CMake target for this file and no `--target cpp_sma_backtest`
    to invoke. `replay::createCsvOhlcvReader(...)` on line 116 is not declared
    anywhere in `include/` or `src/`, so the file will not compile as written.
    Read it for the API shape; supply your own OHLCV reader and your own build
    rule if you want to run it. The runnable equivalents are the Python, Node.js
    and Codon examples linked below.

```cpp
--8<-- "examples/cpp_sma_backtest.cpp"
```

## What it shows

- Inheriting from `flox::Strategy` (not `IStrategy`) — `Strategy` exposes `emitMarketBuy/Sell` which `BacktestRunner` intercepts as signals.
- Using `SymbolContext::position` indirectly via the `_long` / `_short` state — the bookkeeping `BacktestRunner` does in `BacktestResult::computeStats()`.
- Feeding `BacktestRunner::run` an `IMultiSegmentReader` — the same reader interface used for `.floxlog` segments.
- Reading stats off `BacktestResult::computeStats()` — same fields exposed in Python and Node.js.

## Compare to other languages

The Python and Node.js versions of the same crossover live in:

- [Python — backtest & live](python-backtest-vs-live.md) (also runs live via `Runner`)
- [Node.js — backtest & live](node-backtest-vs-live.md)
- [Codon — backtest & live](codon-backtest-vs-live.md)
