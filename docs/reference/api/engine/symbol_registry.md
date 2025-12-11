# SymbolRegistry

`SymbolRegistry` assigns and resolves stable `SymbolId` values for instruments and exposes full metadata (`SymbolInfo`) for fast, type-safe access across the engine. It supports serialization for persistence and replay scenarios.

```cpp
struct SymbolInfo {
  SymbolId           id;
  std::string        exchange;          // e.g. "bybit"
  std::string        symbol;            // e.g. "BTCUSDT" or "BTC-30AUG24-50000-C"
  InstrumentType     type = InstrumentType::Spot;

  std::optional<Price>      strike;     // options only
  std::optional<TimePoint>  expiry;     // options only
  std::optional<OptionType> optionType; // Call | Put (options only)
};

class SymbolRegistry : public ISubsystem {
public:
  // Register by plain (exchange, symbol) – defaults to Spot
  SymbolId registerSymbol(const std::string& exchange,
                          const std::string& symbol);

  // Register with full metadata
  SymbolId registerSymbol(const SymbolInfo& info);

  // Forward lookup
  std::optional<SymbolId> getSymbolId(const std::string& exchange,
                                      const std::string& symbol) const;

  // Full metadata lookup
  std::optional<SymbolInfo> getSymbolInfo(SymbolId id) const;

  // Reverse lookup (exchange, symbol)
  std::pair<std::string, std::string> getSymbolName(SymbolId id) const;

  // Persistence
  bool saveToFile(const std::filesystem::path& path) const;
  bool loadFromFile(const std::filesystem::path& path);

  // Binary serialization
  std::vector<std::byte> serialize() const;
  bool deserialize(std::span<const std::byte> data);

  // Utilities
  void clear();
  std::vector<SymbolInfo> getAllSymbols() const;
  size_t size() const;

private:
  mutable std::mutex _mutex;
  std::vector<SymbolInfo> _symbols;
  std::unordered_map<std::string, SymbolId> _map;
  std::vector<std::pair<std::string, std::string>> _reverse;
};
```

## Purpose

* Provide a thread-safe, bidirectional mapping between human-readable instrument keys and compact numeric `SymbolId`s.
* Expose complete instrument metadata (`SymbolInfo`) for latency-critical components without repeated parsing.
* Support persistence and replay through file and binary serialization.

## Methods

| Method | Description |
|--------|-------------|
| `registerSymbol(exchange, symbol)` | Registers a spot instrument, returns existing `id` if present. |
| `registerSymbol(info)` | Registers any instrument type with full metadata. |
| `getSymbolId(exchange, symbol)` | Forward lookup from `(exchange, symbol)` to `SymbolId`. |
| `getSymbolInfo(id)` | Retrieves `SymbolInfo` for a given `SymbolId`. |
| `getSymbolName(id)` | Reverse lookup from `SymbolId` to `(exchange, symbol)` pair. |
| `saveToFile(path)` | Saves registry to a file for persistence. |
| `loadFromFile(path)` | Loads registry from a file. |
| `serialize()` | Returns binary representation of the registry. |
| `deserialize(data)` | Restores registry from binary data. |
| `clear()` | Clears all registered symbols. |
| `getAllSymbols()` | Returns all registered `SymbolInfo` entries. |
| `size()` | Returns number of registered symbols. |

## Internal Design

* Composite key `"exchange:symbol"` ensures O(1) forward lookups via `_map`.
* `_symbols` is a dense vector for direct `id` indexing.
* `_reverse` provides constant-time reverse mapping without string reconstruction.
* A single mutex protects all structures; registration is rare, lookups are frequent.

## Notes

* `InstrumentType` allows immediate filtering (`Spot`, `Future`, `Inverse`, `Option`) without extra registry calls in hot paths.
* `SymbolId` remains a compact, contiguous 32-bit value suitable for array indices in event buses and order books.
* Option-specific fields (`strike`, `expiry`, `optionType`) are populated only when `type == InstrumentType::Option`.
* Implements `ISubsystem` interface for lifecycle management.

## See Also

* [Common Types](../common.md) — `SymbolId`, `InstrumentType`, `OptionType` definitions
* [EngineConfig](engine_config.md) — Configuration that drives symbol registration
