# SymbolRegistry

`SymbolRegistry` assigns and resolves stable `SymbolId` values for instruments and exposes full metadata (`SymbolInfo`) for fast, type-safe access across the engine.

```cpp
struct SymbolInfo
{
  SymbolId id;
  std::string exchange;
  std::string symbol;
  InstrumentType type = InstrumentType::Spot;
  Price tickSize{Price::fromDouble(0.01)};

  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;
};

class SymbolRegistry : public ISubsystem
{
public:
  // Register by plain (exchange, symbol) – defaults to Spot
  SymbolId registerSymbol(const std::string& exchange, const std::string& symbol);

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

  // Serialization
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
* Support persistence and serialization for replay and data recording scenarios.

## Methods

| Method                 | Description                                                         |
| ---------------------- | ------------------------------------------------------------------- |
| `registerSymbol(str)`  | Registers a spot instrument, returns existing `id` if present.      |
| `registerSymbol(info)` | Registers any instrument type with full metadata.                   |
| `getSymbolId`          | Forward lookup from `(exchange, symbol)` to `SymbolId`.             |
| `getSymbolName`        | Reverse lookup from `SymbolId` to `(exchange, symbol)` string pair. |
| `getSymbolInfo`        | Returns `std::optional<SymbolInfo>` for the given ID.               |
| `saveToFile`           | Persists registry to a binary file.                                 |
| `loadFromFile`         | Loads registry from a binary file.                                  |
| `serialize`            | Returns binary representation as `std::vector<std::byte>`.          |
| `deserialize`          | Restores state from binary data.                                    |
| `clear`                | Removes all registered symbols.                                     |
| `getAllSymbols`        | Returns copy of all registered `SymbolInfo` entries.                |
| `size`                 | Returns the number of registered symbols.                           |

## Internal Design

* Composite key `"exchange:symbol"` ensures O(1) forward lookups via `_map`.
* `_symbols` is a dense 1-based vector for direct `id` indexing (`id = index + 1`).
* `_reverse` provides constant-time reverse mapping without string reconstruction.
* A single mutex protects all structures; registration is rare, lookups are frequent.

## Notes

* Inherits from `ISubsystem` for lifecycle management integration.
* `InstrumentType` allows immediate filtering (`Spot`, `Future`, `Option`) without extra registry calls in hot paths.
* `SymbolId` remains a compact, contiguous 32-bit value suitable for array indices in event buses and order books.
* Option-specific fields (`strike`, `expiry`, `optionType`) are populated only when `type == InstrumentType::Option`.
* `tickSize` stores the minimum price increment for the instrument (default 0.01).
* Persistence methods are useful for storing symbol mappings alongside recorded market data.
* Binary format version 2 includes `tickSize`; version 1 files are read with default tickSize.
