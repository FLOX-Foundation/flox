# Running the FLOX Engine

This guide explains how to initialize and run the FLOX engine by wiring together subsystems, strategies, and connectors.

## Structure

To launch the engine:

1. Construct the required core subsystems (`BookUpdateBus`, `OrderExecutionBus`, etc.)
2. Register symbols using `SymbolRegistry`
3. Instantiate and configure exchange connectors
4. Subscribe strategies and wire their dependencies
5. Pass everything into the `Engine` constructor and call `start()`

## Example

```cpp
EngineConfig config = loadConfig();  // Load from JSON or other source

auto registry = std::make_unique<SymbolRegistry>();
auto bookBus = std::make_unique<BookUpdateBus>();
auto tradeBus = std::make_unique<TradeBus>();
auto orderBus = std::make_unique<OrderExecutionBus>();

ConnectorFactory::instance().registerConnector("bybit",
    [bookBus = bookBus.get(), tradeBus = tradeBus.get(), registry = registry.get()]
    (const std::string& symbolStr) {
      auto symbolId = registry->getSymbolId("bybit", symbolStr);
      auto conn = std::make_shared<BybitExchangeConnector>(symbolStr, *symbolId);
      conn->setCallbacks(
          [bookBus](const BookUpdateEvent& b) { bookBus->publish(b); },
          [tradeBus](const TradeEvent& t) { tradeBus->publish(t); });
      return conn;
    });

std::vector<std::shared_ptr<IExchangeConnector>> connectors;
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
  bookBus->subscribe(strat.get());
  tradeBus->subscribe(strat.get());
}

// Final wiring
subsystems.push_back(std::move(bookBus));
subsystems.push_back(std::move(tradeBus));
subsystems.push_back(std::move(orderBus));
subsystems.push_back(std::move(registry));

Engine engine(config, std::move(subsystems), std::move(connectors));
engine.start();
```

## Notes

* Strategies must implement `IMarketDataSubscriber` and provide `id()` method
* Subsystems must inherit from `ISubsystem`
* Connectors must inherit from `IExchangeConnector`
* Connectors are responsible for publishing `BookUpdateEvent` and `TradeEvent` into their respective buses
* All components must be constructed and wired manually before engine startup

## Lifecycle

The engine will:

1. Start all subsystems (including buses)
2. Start all exchange connectors
3. Begin dispatching events to strategies via `EventBus`
4. Continue running until stopped or externally terminated

Use this pattern to construct simulation environments, test harnesses, or live trading nodes.
