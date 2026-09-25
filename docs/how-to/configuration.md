# Configuration

!!! info "C++ engine setup"
    `EngineConfig` is what the **C++ engine** itself reads at startup. Everything else — which venues to connect, which symbols to subscribe to, the kill switch, the logger — is wired up by constructing the components and handing them to the engine. Python, Node.js, and Codon users build the same thing imperatively (`flox.SymbolRegistry`, `flox.Runner`, etc.) — see the [Bindings](../bindings/README.md) page. This guide is for the C++ entrypoint.

`EngineConfig` is a plain struct with two fields. It is not loaded from JSON by
the engine and there is no serializer for it in the tree; fill it in yourself.

```cpp
#include "flox/engine/engine_config.h"

EngineConfig config;
config.drainTimeoutMs = 5000;
config.memoryProfile = "default";

Engine engine(config, std::move(subsystems), std::move(connectors));
engine.start();
```

## Configuration Fields

| Field            | Type       | Default     | Description                                                                             |
| ---------------- | ---------- | ----------- | --------------------------------------------------------------------------------------- |
| `drainTimeoutMs` | `uint32_t` | 5000        | How long `Engine::stop()` waits for each connector to drain its in-flight orders.        |
| `memoryProfile`  | `string`   | `"default"` | `"default"` leaves pages evictable; `"colo"` calls `mlockall` at start and warns if the privilege is missing. |

## Exchanges and symbols

They are not declared in `EngineConfig`. The struct used to carry an
`exchanges` vector of `ExchangeConfig{name, type, symbols}`, each entry of
`symbols` a `SymbolConfig{symbol, tickSize, expectedDeviation}`, and the engine
read none of it — a caller who filled it in got no registered symbol, no
tick size and no error. The three types have been removed rather than left as a
second, silent way to say the same thing.

A connector is constructed with the venue and the symbols it serves, and
registers what it resolves in a `SymbolRegistry`:

```cpp
SymbolRegistry registry;

SymbolInfo info;
info.exchange = "bybit";
info.symbol = "DOTUSDT";
info.type = InstrumentType::Spot;
info.tickSize = Price::fromDouble(0.001);

const SymbolId id = registry.registerSymbol(info);
```

`SymbolId` is derived from the `(exchange, symbol)` pair, so the same pair
always maps to the same id within a registry, and `registry.getSymbolId(...)`
returns it afterwards. `SymbolInfo::tickSize` is the price resolution the order
book and the validators align to.

## Logging

The logger is an object, not a config field. Build the sink you want and
install it:

```cpp
AtomicLoggerOptions opts;
opts.levelThreshold = LogLevel::Warn;
opts.directory = "/var/log/flox";   // defaults to /dev/shm, or the system temp directory
AtomicLogger logger(opts);
setGlobalLogger(&logger);
```

The level the sink was built with is also the level `FLOX_LOG_*` checks before
it formats anything, so a filtered line costs nothing. See
[`AtomicLogger`](../reference/api/log/atomic_logger.md) and
[the logging macros](../reference/api/log/log.md).

## Kill switch

Also an object. Construct the kill switch with its limits and register it with
the risk path; it is not part of `EngineConfig`.

## Compile-time defaults

`engine_config.h` also defines the constants in `flox::config` — event-bus
capacity, connector pool capacity, order-tracker capacity, the CPU-affinity
priorities — each overridable with a preprocessor define. See
[EngineConfig](../reference/api/engine/engine_config.md) for the full list and
the pool-vs-bus sizing rule.

## Notes

- Nothing in the tree reads `EngineConfig` except `Engine::start()` (the memory profile) and `Engine::stop()` (the drain timeout).
- The config is copied into the engine at construction and is not re-read afterwards.

## See Also

- [Quickstart](../tutorials/quickstart.md) — Build and run FLOX
- [Run the Demo](../tutorials/demo.md) — See configuration in action
- [Architecture](../explanation/architecture.md) — How config affects components
