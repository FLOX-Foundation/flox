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
