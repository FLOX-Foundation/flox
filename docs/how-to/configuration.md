# Configuration

FLOX is configured via the `EngineConfig` structure, typically loaded from a JSON file or embedded configuration source.

## Example Configuration

```json
{
  "logLevel": "debug",
  "exchanges": [
    {
      "name": "bybit",
      "type": "mock",
      "symbols": [
        { "symbol": "DOTUSDT", "tickSize": 0.001, "expectedDeviation": 0.5 }
      ]
    }
  ],
  "killSwitchConfig": {
    "maxOrderQty": 10000,
    "maxLoss": -5000,
    "maxOrdersPerSecond": 100
  }
}
```

## Configuration Fields

### `logLevel`

Controls runtime logging verbosity.

| Value | Description |
|-------|-------------|
| `debug` | All messages including debug info |
| `info` | Informational messages and above |
| `warn` | Warnings and errors only |
| `error` | Errors only |

### `exchanges[]`

Defines which exchange connectors to start and which symbols to subscribe to.

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Display label or unique ID for internal routing |
| `type` | string | Used by `ConnectorFactory` to instantiate the appropriate connector |
| `symbols[]` | array | List of symbol configs |

#### Symbol Configuration

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | string | Trading pair (e.g., "BTCUSDT") |
| `tickSize` | number | Minimum price increment |
| `expectedDeviation` | number | Allowed price deviation for validation |

### `killSwitchConfig`

Defines runtime shutdown thresholds:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `maxOrderQty` | number | 10000 | Maximum order size allowed per submission |
| `maxLoss` | number | -1000000 | Hard limit on realized/unrealized loss (negative) |
| `maxOrdersPerSecond` | number | -1 | Rate limit for outbound orders (`-1` disables) |

### `logFile`

Optional path to log file. If not set, logs go to stdout only.

```json
{
  "logFile": "/var/log/flox/engine.log"
}
```

### `drainTimeoutMs`

Timeout in milliseconds for draining in-flight orders on shutdown. Default: `5000`.

## Loading Configuration

### From JSON File

```cpp
#include "flox/engine/engine_config.h"
#include <fstream>

EngineConfig loadConfig(const std::string& path) {
    std::ifstream file(path);
    nlohmann::json j;
    file >> j;
    return j.get<EngineConfig>();
}

int main() {
    auto config = loadConfig("config.json");
    Engine engine(config, subsystems, connectors);
    engine.start();
}
```

### Programmatic Configuration

```cpp
EngineConfig config;
config.logLevel = LogLevel::INFO;

ExchangeConfig exchange;
exchange.name = "binance";
exchange.type = "binance_futures";

SymbolConfig symbol;
symbol.symbol = "BTCUSDT";
symbol.tickSize = 0.1;
config.exchanges.push_back(exchange);

config.killSwitchConfig.maxOrderQty = 10000;
config.killSwitchConfig.maxLoss = -5000;
config.killSwitchConfig.maxOrdersPerSecond = 100;
```

## Symbol Registration

`SymbolId` is derived automatically from `(exchange, symbol)` during engine startup:

```cpp
// During startup, the engine registers symbols
for (const auto& exchange : config.exchanges) {
    for (const auto& sym : exchange.symbols) {
        auto symbolId = registry.registerSymbol(exchange.name, sym.symbol);
        // symbolId is now available for use throughout the system
    }
}
```

## Environment Variables

Some settings can be overridden via environment variables:

| Variable | Description |
|----------|-------------|
| `FLOX_LOG_LEVEL` | Override log level |
| `FLOX_CONFIG_PATH` | Default config file path |

## Validation

Configuration is validated at startup:

```cpp
void validateConfig(const EngineConfig& config) {
    if (config.exchanges.empty()) {
        throw std::invalid_argument("At least one exchange required");
    }

    for (const auto& ex : config.exchanges) {
        if (ex.symbols.empty()) {
            throw std::invalid_argument("Exchange must have symbols");
        }
        for (const auto& sym : ex.symbols) {
            if (sym.tickSize <= 0) {
                throw std::invalid_argument("tickSize must be positive");
            }
        }
    }
}
```

## Notes

- Tick size and deviation are used by validators and order book alignment
- All configuration is immutable after startup for safety and determinism
- Use different config files for development, testing, and production

## See Also

- [Quickstart](../tutorials/quickstart.md) — Build and run FLOX
- [Run the Demo](../tutorials/demo.md) — See configuration in action
- [Architecture](../explanation/architecture.md) — How config affects components
