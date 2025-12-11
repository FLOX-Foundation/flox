# EngineConfig

`EngineConfig` holds top-level runtime configuration for the trading engine, including exchange definitions, kill switch limits, logging preferences, and compile-time defaults.

```cpp
struct EngineConfig {
  std::vector<ExchangeConfig> exchanges;
  KillSwitchConfig killSwitchConfig;
  std::string logLevel = "info";
  std::string logFile;
};
```

## Purpose

* Aggregate all user-specified engine parameters into a single loadable structure.

## Fields

| Field | Description |
|-------|-------------|
| `exchanges` | List of exchanges and symbols to connect (via `ExchangeConfig`). |
| `killSwitchConfig` | Limits for order size, frequency, and loss (see `KillSwitchConfig`). |
| `logLevel` | Runtime log verbosity (`info`, `debug`, `warn`, `error`). |
| `logFile` | Optional path to write logs to disk. |

## Substructures

### `ExchangeConfig`

```cpp
struct ExchangeConfig {
  std::string name;                    // Display name (e.g. "bybit")
  std::string type;                    // Connector type for factory
  std::vector<SymbolConfig> symbols;   // Symbols to subscribe
};
```

### `SymbolConfig`

```cpp
struct SymbolConfig {
  std::string symbol;          // Symbol name (e.g. "BTCUSDT")
  double tickSize;             // Price resolution
  double expectedDeviation;    // Max allowed distance from center
};
```

### `KillSwitchConfig`

```cpp
struct KillSwitchConfig {
  double maxOrderQty = 10'000.0;    // Per-order size limit
  double maxLoss = -1e6;            // Hard loss cap (negative)
  int maxOrdersPerSecond = -1;      // Rate limit (-1 = disabled)
};
```

## Compile-Time Defaults

The header also defines compile-time constants that can be overridden via preprocessor:

```cpp
// Can be overridden at compile time
#ifndef FLOX_DEFAULT_EVENTBUS_CAPACITY
#define FLOX_DEFAULT_EVENTBUS_CAPACITY 4096
#endif

#ifndef FLOX_DEFAULT_EVENTBUS_MAX_CONSUMERS
#define FLOX_DEFAULT_EVENTBUS_MAX_CONSUMERS 128
#endif

#ifndef FLOX_DEFAULT_ORDER_TRACKER_CAPACITY
#define FLOX_DEFAULT_ORDER_TRACKER_CAPACITY 4096
#endif
```

### `config` Namespace Constants

```cpp
namespace config {
  // EventBus defaults
  inline constexpr size_t DEFAULT_EVENTBUS_CAPACITY = 4096;
  inline constexpr size_t DEFAULT_EVENTBUS_MAX_CONSUMERS = 128;

  // CPU Affinity Priority Constants
  inline constexpr int ISOLATED_CORE_PRIORITY_BOOST = 5;
  inline constexpr int DEFAULT_REALTIME_PRIORITY = 80;
  inline constexpr int FALLBACK_REALTIME_PRIORITY = 90;

  // Component-specific priority constants
  inline constexpr int MARKET_DATA_PRIORITY = 90;
  inline constexpr int EXECUTION_PRIORITY = 85;
  inline constexpr int STRATEGY_PRIORITY = 80;
  inline constexpr int RISK_PRIORITY = 75;
  inline constexpr int GENERAL_PRIORITY = 70;

  // Order tracker capacity
  inline constexpr int ORDER_TRACKER_CAPACITY = 4096;
}
```

| Constant | Value | Description |
|----------|-------|-------------|
| `DEFAULT_EVENTBUS_CAPACITY` | 4096 | Ring buffer size for EventBus |
| `DEFAULT_EVENTBUS_MAX_CONSUMERS` | 128 | Maximum subscribers per bus |
| `ISOLATED_CORE_PRIORITY_BOOST` | 5 | Priority boost for isolated cores |
| `DEFAULT_REALTIME_PRIORITY` | 80 | Default RT priority for threads |
| `FALLBACK_REALTIME_PRIORITY` | 90 | Fallback RT priority |
| `MARKET_DATA_PRIORITY` | 90 | Priority for market data threads |
| `EXECUTION_PRIORITY` | 85 | Priority for execution threads |
| `STRATEGY_PRIORITY` | 80 | Priority for strategy threads |
| `RISK_PRIORITY` | 75 | Priority for risk threads |
| `GENERAL_PRIORITY` | 70 | Priority for general threads |
| `ORDER_TRACKER_CAPACITY` | 4096 | Order tracker hash map capacity |

## Notes

* Typically loaded from JSON during engine bootstrap.
* Used by multiple components: symbol registry, kill switch, connector setup, and logging.
* Priority constants are used by `EventBus` when `FLOX_CPU_AFFINITY_ENABLED` is defined.
* Compile-time defaults can be customized per build configuration.

## See Also

* [Engine](engine.md) — Uses `EngineConfig` for initialization
* [IKillSwitch](../killswitch/abstract_killswitch.md) — Uses `KillSwitchConfig`
* [EventBus](../util/eventing/event_bus.md) — Uses capacity and priority constants
* [Configuration Guide](../../../how-to/configuration.md) — JSON configuration examples
