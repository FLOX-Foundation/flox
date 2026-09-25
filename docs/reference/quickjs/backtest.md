# Backtest & runtime

---

## Simulated executor

```javascript
const executor = new SimulatedExecutor();
executor.submitOrder(id, 'buy', 50000, 1.0, 0, symbolId);
executor.onBar(symbolId, 50100);
executor.advanceClock(tsNs);
executor.fillCount;
```

### The manual bar path

`onBar` moves the market straight to a bar's close, so driving the executor
by hand with it never shows a held order the bar's open or its intrabar
extremes. `onBarOhlc` is what `BacktestRunner` uses internally: it moves the
market to the open first (releasing any order held from a
`beginBarCallbackWindow`/`endBarCallbackWindow` pair at that price), then
walks `low -> high -> close`.

```javascript
executor.advanceClock(60000000000n);
executor.onBarOhlc(symbolId, 50000, 50500, 49800, 50200);

executor.beginBarCallbackWindow();
executor.submitOrder(id, 'buy', 0, 1.0, 1 /* market */, symbolId);
executor.endBarCallbackWindow();
// Held, not matched -- releases at the next onBarOhlc call's open.

executor.reset(); // drop fills before a second hand-driven run
```

---

## Runtime limits

The JS runtime defaults to 32 MB. To change it when embedding via C++:

```cpp
FloxJsEngine engine(64 * 1024 * 1024);  // 64 MB
FloxJsEngine engine(0);                  // no limit
```

---

## IDE support

Copy `quickjs/types/flox.d.ts` and `quickjs/jsconfig.json` into your project directory for VS Code autocomplete.
