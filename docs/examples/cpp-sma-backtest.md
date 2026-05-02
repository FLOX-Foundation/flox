# C++ — SMA backtest

A complete C++ backtest of an SMA(10/30) crossover. Same model as the [Python example](python-backtest-vs-live.md) — one Strategy subclass replayed through `BacktestRunner` against a CSV — but written against the C++ API directly.

```bash
cmake -B build -DFLOX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target cpp_sma_backtest
./build/docs/examples/cpp_sma_backtest docs/examples/data/btcusdt_1m.csv
```

```cpp
--8<-- "examples/cpp_sma_backtest.cpp"
```

## What it shows

- Inheriting from `flox::Strategy` (not `IStrategy`) — `Strategy` exposes `emitMarketBuy/Sell` which `BacktestRunner` intercepts as signals.
- Using `SymbolContext::position` indirectly via the `_long` / `_short` state — the bookkeeping `BacktestRunner` does in `BacktestResult::computeStats()`.
- Loading a CSV via `replay::createCsvOhlcvReader(...)` — the same reader interface used for `.floxlog` segments.
- Reading stats off `BacktestResult::computeStats()` — same fields exposed in Python and Node.js.

## Compare to other languages

The Python and Node.js versions of the same crossover live in:

- [Python — backtest & live](python-backtest-vs-live.md) (also runs live via `Runner`)
- [Node.js — backtest & live](node-backtest-vs-live.md)
- [Codon — backtest & live](codon-backtest-vs-live.md)
