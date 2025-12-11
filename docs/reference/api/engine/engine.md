# Engine

`Engine` coordinates the startup and shutdown of the entire trading system. It owns the engine configuration, all exchange connectors, and system subsystems.

```cpp
class Engine : public ISubsystem {
public:
  Engine(const EngineConfig& config,
         std::vector<std::unique_ptr<ISubsystem>> subsystems,
         std::vector<std::shared_ptr<ExchangeConnector>> connectors);

  void start() override;
  void stop() override;
};
```

## Purpose

* Central orchestration unit responsible for lifecycle management of all core components.

## Responsibilities

| Aspect        | Description                                                        |
| ------------- | ------------------------------------------------------------------ |
| Configuration | Stores full `EngineConfig` used by subsystems and connectors.      |
| Subsystems    | Owns and manages startup/teardown of all registered `ISubsystem`s. |
| Connectors    | Retains exchange adapters to prevent premature destruction.        |


## ExchangeInstance

```cpp
struct ExchangeInstance {
  std::string exchangeType;
  std::string name;
  std::string symbol;
  std::shared_ptr<ExchangeConnector> connector;
};
```

| Field        | Description                                 |
| ------------ | ------------------------------------------- |
| exchangeType | Registered type used in factory resolution. |
| name         | User-defined alias for logging or mapping.  |
| symbol       | Symbol string for routing or filtering.     |
| connector    | Pointer to live exchange connection.        |

## Notes

* `start()`/`stop()` invoke lifecycle methods on all subsystems in the order defined.
* All subsystems are owned via `unique_ptr`; destruction is deterministic and ordered.
* Designed to decouple bootstrapping logic from component logic — the `Engine` is not involved in runtime signal or data flow.
