# ConnectorManager

`ConnectorManager` coordinates lifecycle and callback wiring for multiple `IExchangeConnector` instances, managing startup and routing of market data events.

```cpp
class ConnectorManager
{
public:
  void registerConnector(std::shared_ptr<IExchangeConnector> connector);
  void startAll(IExchangeConnector::BookUpdateCallback onBookUpdate,
                IExchangeConnector::TradeCallback onTrade);

private:
  std::map<std::string, std::shared_ptr<IExchangeConnector>> connectors;
};
```

## Purpose

* Aggregate multiple exchange connectors and manage their startup and event forwarding.

## Responsibilities

| Aspect       | Details                                                                 |
| ------------ | ----------------------------------------------------------------------- |
| Registration | Stores connectors indexed by their `exchangeId()` value.                |
| Startup      | Calls `start()` on all registered connectors.                           |
| Callbacks    | Wires trade and book update callbacks to each connector during startup. |
| Output       | Logs startup of each connector via `FLOX_LOG`.                          |

## Notes

* Assumes connectors are ready to start at the time of `startAll()` — no deferred registration.
* Wraps callbacks using lambdas to allow mutation and move-only semantics.
* Primarily intended for system bootstrap and orchestration; not used in performance-critical paths.
* Callback dispatch remains connector-local; manager only wires them once.
