---
code: E_SYM_001
title: Symbol is not registered
severity: error
since: 0.1.0
---

# E_SYM_001 — Symbol is not registered

A request referenced a symbol name (e.g. `"BTCUSDT"`) that the engine's
`SymbolRegistry` has never seen. lrvx requires every symbol to be
registered up-front so that strategies, indicators, and replay sources
can map names to compact `uint32_t` IDs at hot-path latency.

## Where it fires

- `Engine.run_csv()` and `Engine.run_ohlcv()` when a symbol referenced in
  the input doesn't appear in `add_symbol()` calls.
- `SignalBuilder.market_buy(symbol="…")` and similar emitters.
- Any C-API path that accepts a `const char* symbol` (e.g.
  `lrvx_registry_get_symbol_id`).

## How to fix

Register the symbol **before** running the engine or constructing
signals.

=== "Python"
    ```python
    import lrvx

    eng = lrvx.Engine()
    eng.add_symbol("BTCUSDT")           # ← register first
    eng.run_csv("BTCUSDT", "btc.csv")   # ← then reference it
    ```

=== "Node"
    ```js
    const lrvx = require("@lrvx/lrvx");
    const eng = new lrvx.Engine();
    eng.addSymbol("BTCUSDT");
    ```

=== "C API"
    ```c
    LrvxRegistryHandle reg = lrvx_registry_create();
    lrvx_registry_add_symbol(reg, "exchange", "BTCUSDT", /*tick_size=*/0.01);
    ```

## Common causes

- **Typos** between `add_symbol()` and the symbol referenced later.
  lrvx is case-sensitive; `"BTCUSDT"` and `"btcusdt"` are different
  symbols.
- **Forgetting to register a symbol** that appears in the dataset but
  isn't part of the strategy's declared universe — register every symbol
  you intend to read from, even if you won't trade it.
- **Loading data before registering** — for stream / replay paths that
  need the symbol ID immediately.

## Diagnosing

The `LrvxError` exception carries the offending symbol name in its
`message` field. Print the registered set to compare:

```python
try:
    eng.run_csv("BTCUSDT", "btc.csv")
except lrvx.LrvxError as e:
    print(f"{e.code}: {e.message}")
    print(f"registered: {sorted(eng.symbols())}")
```
