# ConnectorManager

`ConnectorManager` coordinates lifecycle and callback wiring for multiple `IExchangeConnector` instances, managing startup and routing of market data events.

```cpp
class ConnectorManager
{
public:
  ConnectorManager() = default;
  ~ConnectorManager();  // calls stopAll()

  ConnectorManager(const ConnectorManager&) = delete;
  ConnectorManager& operator=(const ConnectorManager&) = delete;

  void registerConnector(std::shared_ptr<IExchangeConnector> connector);
  void startAll(IExchangeConnector::BookUpdateCallback onBookUpdate,
                IExchangeConnector::TradeCallback onTrade);
  void stopAll();

private:
  std::map<std::string, std::shared_ptr<IExchangeConnector>> connectors;
};
```

## Purpose

* Aggregate multiple exchange connectors and manage their startup, shutdown, and event forwarding.

## Responsibilities

| Aspect       | Details                                                                 |
| ------------ | ----------------------------------------------------------------------- |
| Registration | Stores connectors indexed by their `exchangeId()` value.                |
| Startup      | Calls `start()` on all registered connectors.                           |
| Shutdown     | `stopAll()` calls `stop()` on every registered connector; the destructor calls `stopAll()` so a manager going out of scope never leaves a connector running. |
| Callbacks    | Wires trade and book update callbacks to each connector during startup. |
| Output       | Logs startup and shutdown of each connector via `FLOX_LOG`.             |

## Notes

* Assumes connectors are ready to start at the time of `startAll()`. There is no deferred registration.
* `startAll()` takes one `BookUpdateCallback` and one `TradeCallback` and wires that same pair to every registered connector, not a separate callback per connector. Since these callbacks are move-only, each connector's wiring lambda captures a `shared_ptr` to a single shared owner instead of the callback itself.
* Not copyable. A copy would share ownership of the same connectors as the original, and each copy's destructor would call `stop()` on all of them.
* `stopAll()` is safe to call more than once and safe on connectors that were never started.
* Intended for system bootstrap and orchestration, not for performance-critical paths.
* Callback dispatch stays connector-local; the manager only wires the callbacks once.
