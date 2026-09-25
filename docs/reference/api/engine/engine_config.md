# EngineConfig

`EngineConfig` holds what the engine itself needs to start: how long to drain
in-flight orders on shutdown, and the deployment memory profile.

```cpp
struct EngineConfig
{
  uint32_t drainTimeoutMs = 5000;
  std::string memoryProfile = "default";
};
```

## Purpose

* Carry the two runtime parameters `Engine::start()` and `Engine::stop()` read.

## Fields

| Field          | Default     | Description                                                                                 |
| -------------- | ----------- | ------------------------------------------------------------------------------------------- |
| drainTimeoutMs | 5000        | How long `stop()` waits for each connector to drain in-flight orders.                        |
| memoryProfile  | `"default"` | `"default"` (no page locking) or `"colo"` (`mlockall` at start; degrades with a warning).    |

## Exchanges and symbols are not configured here

The struct used to carry `std::vector<ExchangeConfig> exchanges`, with a
`SymbolConfig{symbol, tickSize, expectedDeviation}` under each, and the engine
read none of it: a caller who filled it in got no registered symbol, no tick
size and no error. Those three types are gone.

A connector is constructed with the venue and the symbols it serves, and
registers what it resolves in a [`SymbolRegistry`](./symbol_registry.md) --
which is the one place a configured symbol is observable, and where
`SymbolInfo::tickSize` comes from. Kill-switch limits, log level and log file
were likewise described here and never existed in the struct; configure the
kill switch and the logger through their own objects.

## Global Constants

The header also defines compile-time defaults via `flox::config` namespace:

```cpp
namespace config
{
  inline constexpr size_t DEFAULT_EVENTBUS_CAPACITY = 4096;
  inline constexpr size_t DEFAULT_EVENTBUS_MAX_CONSUMERS = 128;

  // Connector pool capacity (must be > EventBus capacity to prevent exhaustion)
  inline constexpr size_t DEFAULT_CONNECTOR_POOL_CAPACITY = 8191;

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

  // Order tracker capacity: the default bound of a default-constructed
  // OrderTracker. Over it, terminal entries are recycled.
  inline constexpr int ORDER_TRACKER_CAPACITY = 4096;
}
```

These can be overridden via preprocessor defines:

- `FLOX_DEFAULT_EVENTBUS_CAPACITY`
- `FLOX_DEFAULT_EVENTBUS_MAX_CONSUMERS`
- `FLOX_DEFAULT_ORDER_TRACKER_CAPACITY`
- `FLOX_DEFAULT_CONNECTOR_POOL_CAPACITY`

**Important:** `DEFAULT_CONNECTOR_POOL_CAPACITY` must be greater than `DEFAULT_EVENTBUS_CAPACITY`. EventBus only reclaims events on wrap-around, so if pool capacity ≤ bus capacity, the pool will exhaust before any events are returned.

## Notes

* Read by `Engine::start()` (memory profile) and `Engine::stop()` (drain timeout); nothing else in the tree reads it.
* Priority constants are used for CPU affinity and thread scheduling when `FLOX_ENABLE_CPU_AFFINITY` is enabled.
