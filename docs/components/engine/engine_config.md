# EngineConfig

`EngineConfig` holds top-level runtime configuration for the trading engine, including exchange definitions, kill switch limits, and logging preferences.

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

## Responsibilities

| Field            | Description                                                          |
| ---------------- | -------------------------------------------------------------------- |
| exchanges        | List of exchanges and symbols to connect (via `ExchangeConfig`).     |
| killSwitchConfig | Limits for order size, frequency, and loss (see `KillSwitchConfig`). |
| logLevel         | Runtime log verbosity (`info`, `debug`, `trace`, etc.).              |
| logFile          | Optional path to write logs to disk.                                 |


## Substructures

### `ExchangeConfig`

| Field   | Description                                  |
| ------- | -------------------------------------------- |
| name    | Display name or label (e.g. `"Bybit"`).      |
| type    | Connector type (used by `ConnectorFactory`). |
| symbols | List of `SymbolConfig` entries.              |

### `SymbolConfig`

| Field             | Description                              |
| ----------------- | ---------------------------------------- |
| symbol            | Symbol name (e.g. `"DOTUSDT"`).          |
| tickSize          | Price resolution used by the order book. |
| expectedDeviation | Max allowed distance from center price.  |

### `KillSwitchConfig`

| Field              | Description                                         |
| ------------------ | --------------------------------------------------- |
| maxOrderQty        | Per-order size limit.                               |
| maxLoss            | Hard loss cap per session.                          |
| maxOrdersPerSecond | Throttling limit for message rate (≤ 0 = disabled). |

## Notes

* Typically loaded from JSON during engine bootstrap.
* Used by multiple components: symbol registry, kill switch, connector setup, and logging.
