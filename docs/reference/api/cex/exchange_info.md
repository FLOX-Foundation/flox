# ExchangeInfo

Exchange metadata with fixed-size name storage.

## Header

```cpp
#include "flox/exchange/exchange_info.h"
```

## Synopsis

```cpp
struct ExchangeInfo
{
  static constexpr size_t kMaxNameLength = 16;

  std::array<char, kMaxNameLength> name{};
  VenueType type{VenueType::CentralizedExchange};

  [[nodiscard]] std::string_view nameView() const noexcept;
  void setName(std::string_view n) noexcept;
};
```

## VenueType

```cpp
enum class VenueType : uint8_t
{
  CentralizedExchange,  // Binance, Bybit, Kraken, etc
  AmmDex,               // Uniswap, Raydium, etc (future)
  HybridDex             // dYdX, Hyperliquid, etc (future)
};
```

## Design

- **Fixed-size name** - No heap allocation, 16 bytes
- **No `std::string_view` storage** - Avoids dangling reference risk
- **No runtime metrics** - Static info only, metrics tracked separately

## Usage

### Creating ExchangeInfo

```cpp
ExchangeInfo info;
info.setName("Binance");
info.type = VenueType::CentralizedExchange;
```

### Via SymbolRegistry

```cpp
SymbolRegistry registry;

// Register returns ExchangeId (uint8_t)
ExchangeId id = registry.registerExchange("Binance", VenueType::CentralizedExchange);

// Get info by ID
const ExchangeInfo* info = registry.getExchange(id);
std::cout << "Name: " << info->nameView() << "\n";
std::cout << "Type: " << static_cast<int>(info->type) << "\n";
```

### Name Truncation

Names longer than `kMaxNameLength - 1` (15 characters) are truncated:

```cpp
info.setName("VeryLongExchangeName");
// info.nameView() == "VeryLongExchang" (15 chars)
```

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `kMaxNameLength` | 16 | Maximum name storage (15 chars + null) |
| `InvalidExchangeId` | 255 | Sentinel value for invalid exchange |

## Memory Layout

```cpp
struct ExchangeInfo {
  std::array<char, 16> name;  // 16 bytes
  VenueType type;             // 1 byte
  // 15 bytes padding (to 32-byte alignment if needed)
};
```

Total size: 17 bytes (plus padding).

## See Also

- [SymbolRegistry](../engine/symbol_registry.md) - Exchange and symbol management
- [CEX Overview](index.md) - Cross-exchange coordination
