# Running the Flox Engine

This guide explains how to initialize and run the Flox engine by wiring together subsystems, strategies, and connectors.

## Structure

To launch the engine:

1. Construct the required core subsystems (`MarketDataBus`, `OrderExecutionBus`, etc.)
2. Register symbols using `SymbolRegistry`
3. Instantiate and configure exchange connectors
4. Subscribe strategies and wire their dependencies
5. Pass everything into the `Engine` constructor and call `start()`

## Example

```cpp
EngineConfig config = loadConfig();  // Load from JSON or other source

auto registry = std::make_unique<SymbolRegistry>();
auto mdb = std::make_unique<MarketDataBus>();
auto orderBus = std::make_unique<OrderExecutionBus>();

ConnectorFactory::instance().registerConnector("bybit",
  [mdb = mdb.get(), registry = registry.get()](const std::string& symbolStr) {
    auto symbolId = registry->getSymbolId("bybit", symbolStr);
    auto conn = std::make_shared<BybitExchangeConnector>(symbolStr, *symbolId);
    conn->setCallbacks(
      [mdb](EventHandle<BookUpdateEvent> b) { mdb->publish(std::move(b)); },
      [mdb](const TradeEvent& t) { mdb->publish(t); });
    return conn;
  });

std::vector<std::shared_ptr<ExchangeConnector>> connectors;
std::vector<std::unique_ptr<ISubsystem>> subsystems;

// Register symbols and create connectors
for (const auto& ex : config.exchanges) {
  for (const auto& sym : ex.symbols) {
    registry->registerSymbol(ex.name, sym.symbol);
    auto conn = ConnectorFactory::instance().createConnector(ex.name, sym.symbol);
    if (conn) connectors.push_back(conn);
  }
}

// Load and wire strategies
std::vector<std::shared_ptr<IStrategy>> strategies = loadStrategiesFromConfig(registry.get());
for (const auto& strat : strategies) {
  auto executor = std::make_unique<SimpleOrderExecutor>(*orderBus);
  strat->setOrderExecutor(executor.get());
  subsystems.push_back(std::move(executor));
  mdb->subscribe(strat);
}

// Final wiring
subsystems.push_back(std::move(mdb));
subsystems.push_back(std::move(orderBus));
subsystems.push_back(std::make_unique<Subsystem<SymbolRegistry>>(std::move(registry)));

Engine engine(config, std::move(subsystems), std::move(connectors));
engine.start();
```

## Notes

* Strategies must implement `IMarketDataSubscriber`
* Subsystems must inherit from `ISubsystem` or be wrapped in `Subsystem<T>`
* Connectors are responsible for publishing `BookUpdateEvent` and `TradeEvent` into the bus
* All components must be constructed and wired manually before engine startup

## Lifecycle

The engine will:

1. Start all subsystems
2. Start all exchange connectors
3. Begin dispatching events to strategies via `MarketDataBus`
4. Continue running until stopped or externally terminated

Use this pattern to construct simulation environments, test harnesses, or live trading nodes.
