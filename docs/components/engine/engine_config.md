# EngineConfig

`EngineConfig` holds top-level runtime configuration for the trading engine, including exchange definitions, kill switch limits, and logging preferences.

```cpp
struct EngineConfig
{
  std::vector<ExchangeConfig> exchanges;
  KillSwitchConfig killSwitchConfig;
  std::string logLevel = "info";
  std::string logFile;
};
```

## Purpose

* Aggregate all user-specified engine parameters into a single loadable structure.

## Fields

| Field            | Description                                                          |
| ---------------- | -------------------------------------------------------------------- |
| exchanges        | List of exchanges and symbols to connect (via `ExchangeConfig`).     |
| killSwitchConfig | Limits for order size, frequency, and loss (see `KillSwitchConfig`). |
| logLevel         | Runtime log verbosity (`info`, `debug`, `trace`, etc.).              |
| logFile          | Optional path to write logs to disk.                                 |


## Substructures

### `ExchangeConfig`

```cpp
struct ExchangeConfig
{
  std::string name;
  std::string type;
  std::vector<SymbolConfig> symbols;
};
```

| Field   | Description                                  |
| ------- | -------------------------------------------- |
| name    | Display name or label (e.g. `"Bybit"`).      |
| type    | Connector type (used by `ConnectorFactory`). |
| symbols | List of `SymbolConfig` entries.              |

### `SymbolConfig`

```cpp
struct SymbolConfig
{
  std::string symbol;
  double tickSize;
  double expectedDeviation;
};
```

| Field             | Description                              |
| ----------------- | ---------------------------------------- |
| symbol            | Symbol name (e.g. `"DOTUSDT"`).          |
| tickSize          | Price resolution used by the order book. |
| expectedDeviation | Max allowed distance from center price.  |

### `KillSwitchConfig`

```cpp
struct KillSwitchConfig
{
  double maxOrderQty = 10'000.0;
  double maxLoss = -1e6;
  int maxOrdersPerSecond = -1;
};
```

| Field              | Default   | Description                                         |
| ------------------ | --------- | --------------------------------------------------- |
| maxOrderQty        | 10,000    | Per-order size limit.                               |
| maxLoss            | -1,000,000| Hard loss cap per session.                          |
| maxOrdersPerSecond | -1        | Throttling limit for message rate (≤ 0 = disabled). |

## Global Constants

The header also defines compile-time defaults via `flox::config` namespace:

```cpp
namespace config
{
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

These can be overridden via preprocessor defines:
- `FLOX_DEFAULT_EVENTBUS_CAPACITY`
- `FLOX_DEFAULT_EVENTBUS_MAX_CONSUMERS`
- `FLOX_DEFAULT_ORDER_TRACKER_CAPACITY`

## Notes

* Typically loaded from JSON during engine bootstrap.
* Used by multiple components: symbol registry, kill switch, connector setup, and logging.
* Priority constants are used for CPU affinity and thread scheduling when `FLOX_ENABLE_CPU_AFFINITY` is enabled.
