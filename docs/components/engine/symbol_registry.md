# SymbolRegistry

`SymbolRegistry` assigns and resolves stable `SymbolId` values for `(exchange, symbol)` pairs. It enables compact, integer-based symbol references throughout the engine.

```cpp
class SymbolRegistry {
public:
  SymbolId registerSymbol(const std::string& exchange, const std::string& symbol);
  std::optional<SymbolId> getSymbolId(const std::string& exchange, const std::string& symbol) const;
  std::pair<std::string, std::string> getSymbolName(SymbolId id) const;

private:
  std::unordered_map<std::string, SymbolId> _map;
  std::vector<std::pair<std::string, std::string>> _reverse;
  mutable std::mutex _mutex;
};
```

## Purpose

* Provide a fast, thread-safe mapping between human-readable `(exchange, symbol)` and numeric `SymbolId`.

## Responsibilities

| Method           | Description                                                     |
| ---------------- | --------------------------------------------------------------- |
| `registerSymbol` | Assigns a new `SymbolId` if not already registered.             |
| `getSymbolId`    | Returns the `SymbolId` for a given `(exchange, symbol)`.        |
| `getSymbolName`  | Resolves a `SymbolId` back to `(exchange, symbol)` string pair. |

## Internal Design

* `_map` uses a composite string key: `"exchange:symbol" → SymbolId`.
* `_reverse` holds a parallel vector of `(exchange, symbol)` for reverse lookup.
* All methods are guarded by a single mutex for thread safety.

## Notes

* Used by all market data components to route and normalize per-symbol state.
* `SymbolId` is a dense, zero-based `uint32_t`, suitable for array indexing.
* Forward and reverse lookups are both O(1) and memory-efficient.
