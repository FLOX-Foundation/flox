# Engine & Lifecycle Reference

Core engine, subsystems, and configuration.

## Engine

**Header:** `flox/engine/engine.h`

The `Engine` class orchestrates the system lifecycle.

```cpp
class Engine : public ISubsystem
{
public:
  Engine(const EngineConfig& config,
         std::vector<std::unique_ptr<ISubsystem>> subsystems,
         std::vector<std::shared_ptr<IExchangeConnector>> connectors);

  void start() override;
  void stop() override;
};
```

### Lifecycle

1. `Engine::start()`:
   - Starts all subsystems (buses, strategies, etc.)
   - Starts all connectors

2. `Engine::stop()`:
   - Stops all connectors first
   - Stops all subsystems

### Example

```cpp
EngineConfig config{};
std::vector<std::unique_ptr<ISubsystem>> subsystems;
std::vector<std::shared_ptr<IExchangeConnector>> connectors;

// Add subsystems and connectors...

Engine engine(config, std::move(subsystems), std::move(connectors));
engine.start();
// ... run ...
engine.stop();
```

---

## ISubsystem

**Header:** `flox/engine/abstract_subsystem.h`

Base interface for all lifecycle-managed components.

```cpp
class ISubsystem
{
public:
  virtual ~ISubsystem() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
};
```

### Implementations

| Class | Purpose |
|-------|---------|
| `Engine` | Top-level orchestrator |
| `EventBus<T>` | Event delivery |
| `IStrategy` | Trading strategies |
| `BarAggregator` | OHLCV bar aggregation |

---

## ISubscriber

**Header:** `flox/engine/abstract_subscriber.h`

Base interface for event consumers.

```cpp
using SubscriberId = uintptr_t;

class ISubscriber
{
public:
  virtual ~ISubscriber() = default;
  virtual SubscriberId id() const = 0;
};
```

### Implementation Pattern

```cpp
class MySubscriber : public ISubscriber
{
public:
  SubscriberId id() const override {
    return reinterpret_cast<SubscriberId>(this);
  }
};
```

---

## IMarketDataSubscriber

**Header:** `flox/engine/abstract_market_data_subscriber.h`

Receives market data events.

```cpp
class IMarketDataSubscriber : public ISubscriber
{
public:
  virtual ~IMarketDataSubscriber() = default;

  virtual void onBookUpdate(const BookUpdateEvent& ev) {}
  virtual void onTrade(const TradeEvent& ev) {}
  virtual void onBar(const BarEvent& ev) {}
};
```

### Notes

- All callbacks have empty default implementations
- Override only the events you need
- Filter by `symbolId` in your implementation

---

## EngineConfig

**Header:** `flox/engine/engine_config.h`

Configuration structure for the engine.

```cpp
struct EngineConfig
{
  std::vector<ExchangeConfig> exchanges;
  KillSwitchConfig killSwitchConfig;
  std::string logLevel = "info";
  std::string logFile;
};

struct ExchangeConfig
{
  std::string name;
  std::string type;
  std::vector<SymbolConfig> symbols;
};

struct SymbolConfig
{
  std::string symbol;
  double tickSize;
  double expectedDeviation;
};

struct KillSwitchConfig
{
  double maxOrderQty = 10'000.0;
  double maxLoss = -1e6;
  int maxOrdersPerSecond = -1;
};
```

---

## Configuration Constants

**Header:** `flox/engine/engine_config.h`

```cpp
namespace flox::config {

// EventBus defaults
inline constexpr size_t DEFAULT_EVENTBUS_CAPACITY = 4096;
inline constexpr size_t DEFAULT_EVENTBUS_MAX_CONSUMERS = 128;

// CPU affinity priorities
inline constexpr int ISOLATED_CORE_PRIORITY_BOOST = 5;
inline constexpr int DEFAULT_REALTIME_PRIORITY = 80;
inline constexpr int FALLBACK_REALTIME_PRIORITY = 90;

// Component-specific priorities
inline constexpr int MARKET_DATA_PRIORITY = 90;
inline constexpr int EXECUTION_PRIORITY = 85;
inline constexpr int STRATEGY_PRIORITY = 80;
inline constexpr int RISK_PRIORITY = 75;
inline constexpr int GENERAL_PRIORITY = 70;

// Order tracker
inline constexpr int ORDER_TRACKER_CAPACITY = 4096;

}
```

Override at compile time:

```bash
cmake .. -DFLOX_DEFAULT_EVENTBUS_CAPACITY=8192
```

---

## SymbolRegistry

**Header:** `flox/engine/symbol_registry.h`

Maps exchange+symbol strings to `SymbolId`.

```cpp
class SymbolRegistry
{
public:
  SymbolId registerSymbol(const std::string& exchange, const std::string& symbol);
  std::optional<SymbolId> getSymbolId(const std::string& exchange, const std::string& symbol) const;
  std::optional<std::pair<std::string, std::string>> getSymbolName(SymbolId id) const;
  size_t size() const;
};
```

### Example

```cpp
SymbolRegistry registry;
registry.registerSymbol("binance", "BTCUSDT");
registry.registerSymbol("binance", "ETHUSDT");

auto id = registry.getSymbolId("binance", "BTCUSDT");  // Returns 0
auto name = registry.getSymbolName(0);  // Returns {"binance", "BTCUSDT"}
```

---

## Common Types

**Header:** `flox/common.h`

### Type Aliases

```cpp
using SymbolId = uint32_t;
using OrderId = uint64_t;
```

### Fixed-Point Types

```cpp
// Tick = 0.000001 (6 decimal places)
using Price = Decimal<PriceTag, 1'000'000, 1>;
using Quantity = Decimal<QuantityTag, 1'000'000, 1>;
using Volume = Decimal<VolumeTag, 1'000'000, 1>;
```

### Enums

```cpp
enum class Side { BUY, SELL };

enum class OrderType { LIMIT, MARKET };

enum class InstrumentType { Spot, Future, Inverse, Option };

enum class OptionType { CALL, PUT };
```

---

## EventDispatcher

**Header:** `flox/engine/event_dispatcher.h`

Internal template for dispatching events to listeners.

```cpp
template <typename Event>
struct EventDispatcher
{
  static void dispatch(const Event& ev, IMarketDataSubscriber& sub);
};
```

Specializations exist for:
- `TradeEvent` → calls `onTrade()`
- `BookUpdateEvent` → calls `onBookUpdate()`
- `BarEvent` → calls `onBar()`
- `pool::Handle<T>` → unwraps handle and dispatches

---

## See Also

- [Market Data Reference](market-data.md) — Events and buses
- [Architecture Overview](../explanation/architecture.md) — System design
