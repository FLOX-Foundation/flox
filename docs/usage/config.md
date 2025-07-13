# Configuration

Flox is configured via the `EngineConfig` structure, typically loaded from a JSON file or embedded configuration source.

## Example

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

## Fields

### `logLevel`

Controls runtime logging verbosity (`debug`, `info`, `warn`, etc.)

### `exchanges[]`

Defines which exchange connectors to start and which symbols to subscribe to.

* `name`: display label or unique ID for internal routing
* `type`: used by `ConnectorFactory` to instantiate the appropriate connector
* `symbols[]`: list of symbol configs with tick size and allowed deviation

### `killSwitchConfig`

Defines runtime shutdown thresholds:

* `maxOrderQty`: maximum order size allowed per submission
* `maxLoss`: hard limit on realized/unrealized loss
* `maxOrdersPerSecond`: rate limit for outbound orders (`-1` disables)

## Notes

* `SymbolId` is derived automatically from `(exchange, symbol)` during engine startup
* Tick size and deviation are used by validators and order book alignment
* All configuration is immutable after startup for safety and determinism
