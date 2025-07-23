# SymbolRegistry

`SymbolRegistry` assigns and resolves stable `SymbolId` values for instruments and exposes full metadata (`SymbolInfo`) for fast, type-safe access across the engine.

```cpp
struct SymbolInfo {
    SymbolId           id{};
    std::string        exchange;          // e.g. "bybit"
    std::string        symbol;            // e.g. "BTCUSDT" or "BTC-30AUG24-50000-C"
    InstrumentType     type = InstrumentType::Spot;

    std::optional<Price>      strike;     // options only
    std::optional<TimePoint>  expiry;     // options only
    std::optional<OptionType> optionType; // Call | Put (options only)
};

class SymbolRegistry {
public:
    // Register by plain (exchange, symbol) – defaults to Spot
    SymbolId registerSymbol(const std::string& exchange,
                            const std::string& symbol);

    // Register with full metadata
    SymbolId registerSymbol(const SymbolInfo& info);

    // Forward lookup
    std::optional<SymbolId> getSymbolId(const std::string& exchange,
                                        const std::string& symbol) const;

    // Reverse lookup (exchange, symbol)
    std::pair<std::string, std::string> getSymbolName(SymbolId id) const;

    // Full metadata lookup
    const SymbolInfo* getSymbolInfo(SymbolId id) const;

private:
    mutable std::mutex                 _mutex;
    std::vector<SymbolInfo>            _symbols;  // id-indexed (1-based)
    std::unordered_map<std::string,
                       SymbolId>       _map;      // "exchange:symbol" → id
    std::vector<std::pair<std::string,
                          std::string>> _reverse;  // id → (exchange,symbol)
};
```

## Purpose

* Provide a thread-safe, bidirectional mapping between human-readable instrument keys and compact numeric `SymbolId`s.
* Expose complete instrument metadata (`SymbolInfo`) for latency-critical components without repeated parsing.

## Responsibilities

| Method                 | Description                                                         |
| ---------------------- | ------------------------------------------------------------------- |
| `registerSymbol(str)`  | Registers a spot instrument, returns existing `id` if present.      |
| `registerSymbol(info)` | Registers any instrument type with full metadata.                   |
| `getSymbolId`          | Forward lookup from `(exchange, symbol)` to `SymbolId`.             |
| `getSymbolName`        | Reverse lookup from `SymbolId` to `(exchange, symbol)` string pair. |
| `getSymbolInfo`        | Retrieves immutable pointer to stored `SymbolInfo` for fast access. |

## Internal Design

* Composite key `"exchange:symbol"` ensures O(1) forward lookups via `_map`.
* `_symbols` is a dense 1-based vector for direct `id` indexing (`id = index + 1`).
* `_reverse` provides constant-time reverse mapping without string reconstruction.
* A single mutex protects all structures; registration is rare, lookups are frequent and lock-free reads dominate.

## Notes

* `InstrumentType` allows immediate filtering (`Spot`, `Future`, `Option`) without extra registry calls in hot paths.
* `SymbolId` remains a compact, contiguous 32-bit value suitable for array indices in event buses and order books.
* Option-specific fields (`strike`, `expiry`, `optionType`) are populated only when `type == InstrumentType::Option`.
