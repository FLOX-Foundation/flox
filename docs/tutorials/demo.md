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
cmake .. -DFLOX_BUILD_DEMO=ON
make -j$(nproc)
```

## Run the Demo

```bash
./demo/flox_demo
```

The demo will:

1. Start three synthetic connectors (`demoA`, `demoB`, `demoC`) and
   eight `DemoStrategy` instances, one per symbol id `0..7`
2. Publish market data via `TradeBus` and `BookUpdateBus`
3. Run the strategy and supporting systems for 30 seconds
4. Stop all components and print the latency report

## Expected Output

`main.cpp` calls `FLOX_LOG_OFF()` before `engine->start()` and only
re-enables logging after `engine->stop()`, so the run itself is
silent. What you see is the report:

```
demo finished
[latency] bus_publish | count=... mean=...ns p50=...ns p95=...ns max=...ns
[latency] strategy_onTrade | count=... mean=...ns p50=...ns p95=...ns max=...ns
[latency] execution_onOrderFilled | count=... mean=...ns p50=...ns p95=...ns max=...ns
[latency] end_to_end | count=... mean=...ns p50=...ns p95=...ns max=...ns
```

To see the per-event log lines, edit `NO_COUT` in `demo/src/main.cpp`.

## Code Structure

```
demo/
├── src/
│   ├── main.cpp               # Entry point — build, start, sleep 30s, stop, report
│   ├── demo_builder.cpp       # Wires all components
│   ├── demo_connector.cpp     # Synthetic market data
│   └── demo_strategy.cpp      # Example strategy
├── include/demo/
│   ├── demo_builder.h
│   ├── demo_connector.h
│   ├── demo_strategy.h
│   ├── pairs_strategy.h
│   ├── latency_collector.h    # p50/p95/max latency report
│   └── simple_components.h    # SimpleOrderExecutor, SimplePnLTracker,
│                              # SimpleKillSwitch, SimpleRiskManager, ...
└── data/sample.floxlog
```

`demo/CMakeLists.txt` also builds seven other executables from the
same directory: `multi_timeframe_demo`, `volume_profile_demo`,
`footprint_demo`, `market_profile_demo`, `cex_demo`, plus
`backtest_demo` and `grid_search_demo` when
`FLOX_ENABLE_BACKTEST=ON`.

## Understanding the Demo

### DemoBuilder

The builder demonstrates how to wire FLOX components:

```cpp
// Create buses
auto tradeBus = std::make_unique<TradeBus>();
auto bookBus = std::make_unique<BookUpdateBus>();
auto execBus = std::make_unique<OrderExecutionBus>();

// Create connectors — (id, symbol, bookUpdateBus, tradeBus)
auto connector = std::make_shared<DemoConnector>("demoA", symbolId,
                                                 *bookBus, *tradeBus);

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
