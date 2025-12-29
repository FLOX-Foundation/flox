# SymbolRegistry

`SymbolRegistry` assigns and resolves stable `SymbolId` and `ExchangeId` values for instruments and exchanges. Exposes full metadata (`SymbolInfo`, `ExchangeInfo`) for fast, type-safe access across the engine.

```cpp
struct SymbolInfo
{
  SymbolId id{0};
  std::string exchange;
  std::string symbol;
  InstrumentType type = InstrumentType::Spot;
  Price tickSize{Price::fromDouble(0.01)};
  ExchangeId exchangeId{InvalidExchangeId};

  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;
};

class SymbolRegistry : public ISubsystem
{
public:
  static constexpr size_t kMaxExchanges = 32;
  static constexpr size_t kMaxSymbols = 4096;
  static constexpr size_t kMaxEquivalentsPerSymbol = 8;

  // Exchange management
  ExchangeId registerExchange(std::string_view name,
                              VenueType type = VenueType::CentralizedExchange);
  const ExchangeInfo* getExchange(ExchangeId id) const;
  ExchangeId getExchangeId(std::string_view name) const;
  size_t exchangeCount() const;

  // Symbol registration (legacy string-based API)
  SymbolId registerSymbol(const std::string& exchange, const std::string& symbol);
  SymbolId registerSymbol(const SymbolInfo& info);
  std::optional<SymbolId> getSymbolId(const std::string& exchange,
                                      const std::string& symbol) const;
  std::optional<SymbolInfo> getSymbolInfo(SymbolId id) const;
  std::pair<std::string, std::string> getSymbolName(SymbolId id) const;

  // Symbol registration (ExchangeId-based API)
  SymbolId registerSymbol(ExchangeId exchange, std::string_view symbol);
  ExchangeId getExchangeForSymbol(SymbolId symbol) const;

  // Symbol equivalence (cross-exchange mapping)
  void mapEquivalentSymbols(std::span<const SymbolId> equivalentSymbols);
  std::span<const SymbolId> getEquivalentSymbols(SymbolId symbol) const;
  SymbolId getEquivalentOnExchange(SymbolId symbol, ExchangeId exchange) const;

  // Persistence
  bool saveToFile(const std::filesystem::path& path) const;
  bool loadFromFile(const std::filesystem::path& path);
  std::vector<std::byte> serialize() const;
  bool deserialize(std::span<const std::byte> data);

  // Utilities
  void clear();
  std::vector<SymbolInfo> getAllSymbols() const;
  size_t size() const;
};
```

## Purpose

* Provide a thread-safe, bidirectional mapping between human-readable instrument keys and compact numeric `SymbolId`s.
* Manage exchange registrations with `ExchangeId` for multi-exchange scenarios.
* Support cross-exchange symbol equivalence for arbitrage and routing.
* Expose complete instrument metadata (`SymbolInfo`) for latency-critical components without repeated parsing.
* Support persistence and serialization for replay and data recording scenarios.

## Exchange Management

| Method            | Description                                           |
| ----------------- | ----------------------------------------------------- |
| `registerExchange`| Registers an exchange, returns `ExchangeId`.          |
| `getExchange`     | Returns `ExchangeInfo*` for the given ID.             |
| `getExchangeId`   | Lookup `ExchangeId` by name.                          |
| `exchangeCount`   | Returns number of registered exchanges.               |

## Symbol Registration

| Method                 | Description                                                         |
| ---------------------- | ------------------------------------------------------------------- |
| `registerSymbol(str)`  | Registers a spot instrument, returns existing `id` if present.      |
| `registerSymbol(info)` | Registers any instrument type with full metadata.                   |
| `registerSymbol(ExchangeId, symbol)` | Registers using `ExchangeId` instead of string.      |
| `getSymbolId`          | Forward lookup from `(exchange, symbol)` to `SymbolId`.             |
| `getSymbolName`        | Reverse lookup from `SymbolId` to `(exchange, symbol)` string pair. |
| `getSymbolInfo`        | Returns `std::optional<SymbolInfo>` for the given ID.               |
| `getExchangeForSymbol` | Returns `ExchangeId` for a given symbol.                            |

## Symbol Equivalence

For cross-exchange trading, symbols on different exchanges can be mapped as equivalent:

```cpp
// Register symbols on different exchanges
auto btcBinance = registry.registerSymbol("binance", "BTCUSDT");
auto btcBybit = registry.registerSymbol("bybit", "BTCUSDT");
auto btcOkx = registry.registerSymbol("okx", "BTC-USDT");

// Map them as equivalent
registry.mapEquivalentSymbols({btcBinance, btcBybit, btcOkx});

// Query equivalents
auto equivalents = registry.getEquivalentSymbols(btcBinance);  // Returns all 3
auto bybitEquiv = registry.getEquivalentOnExchange(btcBinance, bybitExchangeId);
```

| Method                    | Description                                               |
| ------------------------- | --------------------------------------------------------- |
| `mapEquivalentSymbols`    | Links symbols as equivalent across exchanges.             |
| `getEquivalentSymbols`    | Returns span of all equivalent symbols.                   |
| `getEquivalentOnExchange` | Returns equivalent symbol on a specific exchange.         |

## Persistence

| Method          | Description                                         |
| --------------- | --------------------------------------------------- |
| `saveToFile`    | Persists registry to a binary file.                 |
| `loadFromFile`  | Loads registry from a binary file.                  |
| `serialize`     | Returns binary representation as `std::vector<std::byte>`. |
| `deserialize`   | Restores state from binary data.                    |

## Capacity Limits

| Constant                   | Value | Description                          |
| -------------------------- | ----- | ------------------------------------ |
| `kMaxExchanges`            | 32    | Maximum number of exchanges.         |
| `kMaxSymbols`              | 4096  | Maximum number of symbols.           |
| `kMaxEquivalentsPerSymbol` | 8     | Maximum equivalents per symbol.      |

## Internal Design

* Composite key `"exchange:symbol"` ensures O(1) forward lookups via `_map`.
* `_symbols` is an unordered_map for direct `id` lookups.
* `_symbolToExchange` provides O(1) symbol-to-exchange mapping.
* Equivalence uses flat storage: `[sym * kMaxEquivalentsPerSymbol ... (sym+1) * kMaxEquivalentsPerSymbol)` for O(1) lookup.
* A single mutex protects all structures; registration is rare, lookups are frequent.

## Notes

* Inherits from `ISubsystem` for lifecycle management integration.
* `InstrumentType` allows immediate filtering (`Spot`, `Future`, `Option`) without extra registry calls in hot paths.
* `SymbolId` remains a compact, contiguous 32-bit value suitable for array indices in event buses and order books.
* Option-specific fields (`strike`, `expiry`, `optionType`) are populated only when `type == InstrumentType::Option`.
* `tickSize` stores the minimum price increment for the instrument (default 0.01).
* `exchangeId` links symbol directly to its exchange for O(1) access.
* Persistence methods are useful for storing symbol mappings alongside recorded market data.
