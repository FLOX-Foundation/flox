# Run the Demo

The `demo` folder provides a minimal working example that wires FLOX components into a functioning system. It demonstrates the architecture, event flow, and subsystem lifecycle in a controlled, simulated environment.

## Features

| Component | Description |
|-----------|-------------|
| `DemoConnector` | Emits synthetic trades and book updates for testing |
| `DemoStrategy` | Receives market data and generates mock orders |
| `SimpleOrderExecutor` | Processes orders and triggers fills via `OrderExecutionBus` |
| `SimplePnLTracker` | Tracks profit and loss |
| `SimpleKillSwitch` | Emergency shutdown control |
| `SimpleRiskManager` | Basic risk controls |
| `DemoBuilder` | Constructs and wires all required subsystems and buses |

## Build the Demo

```bash
cmake .. -DFLOX_ENABLE_DEMO=ON
make -j$(nproc)
```

## Run the Demo

```bash
./demo/flox_demo
```

The demo will:

1. Start two synthetic connectors
2. Publish market data via `TradeBus` and `BookUpdateBus`
3. Run the strategy and supporting systems for approximately five seconds
4. Stop all components and exit cleanly

## Expected Output

```
[INFO] Starting DemoConnector (exchange1)
[INFO] Starting DemoConnector (exchange2)
[INFO] DemoStrategy: received trade BTCUSDT @ 50000.00
[INFO] DemoStrategy: received book update BTCUSDT (10 levels)
[INFO] DemoStrategy: placing order BUY 0.1 BTCUSDT @ 49990.00
[INFO] SimpleOrderExecutor: order filled
...
[INFO] Stopping all components
[INFO] Demo completed
```

## Code Structure

```
demo/
├── main.cpp              # Entry point
├── demo_builder.h        # Wires all components
├── demo_connector.h      # Synthetic market data
├── demo_strategy.h       # Example strategy
└── simple_*.h            # Simple implementations of interfaces
```

## Understanding the Demo

### DemoBuilder

The builder demonstrates how to wire FLOX components:

```cpp
// Create buses
auto tradeBus = std::make_unique<TradeBus>();
auto bookBus = std::make_unique<BookUpdateBus>();
auto execBus = std::make_unique<OrderExecutionBus>();

// Create connectors
auto connector = std::make_shared<DemoConnector>(registry, tradeBus, bookBus);

// Create strategy
auto strategy = std::make_unique<DemoStrategy>();

// Subscribe strategy to buses
tradeBus->subscribe(strategy.get());
bookBus->subscribe(strategy.get());

// Wire execution
strategy->setExecutor(executor.get());

// Start engine
engine.start();
```

### DemoConnector

Generates synthetic market data at configurable rates:

```cpp
void DemoConnector::run() {
    while (_running) {
        TradeEvent trade;
        trade.trade.symbol = _symbolId;
        trade.trade.price = generatePrice();
        trade.trade.quantity = generateQty();

        _tradeBus.publish(trade);

        std::this_thread::sleep_for(10ms);
    }
}
```

### DemoStrategy

Shows how to consume events and place orders:

```cpp
void DemoStrategy::onTrade(const TradeEvent& event) {
    // Process trade
    if (shouldBuy(event)) {
        Order order;
        order.symbol = event.trade.symbol;
        order.side = OrderSide::BUY;
        order.price = event.trade.price - _tickSize;
        order.quantity = _orderSize;

        _executor->submit(order);
    }
}
```

## Notes

- This demo is intended for integration testing and illustration only
- Production deployments should define their own builder and execution harness
- All demo components are isolated and can be replaced with real implementations

## See Also

- [Quickstart](quickstart.md) — Build FLOX from source
- [First Strategy](first-strategy.md) — Write your own strategy
- [Architecture](../explanation/architecture.md) — How components fit together
