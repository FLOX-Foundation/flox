# Demo Application

The `demo` folder provides a minimal working example that wires Flox components into a functioning system. It demonstrates the architecture, event flow, and subsystem lifecycle in a controlled, simulated environment.

## Features

* `DemoConnector`: emits synthetic trades and book updates for testing
* `DemoStrategy`: receives market data and generates mock orders
* `SimpleOrderExecutor`: processes orders and triggers fills via `OrderExecutionBus`
* `SimplePnLTracker`, `SimpleKillSwitch`, `SimpleRiskManager`: lightweight control modules
* `DemoBuilder`: constructs and wires all required subsystems and buses

## Running the Demo

After building the project with CMake:

```bash
./demo/flox_demo
```

The demo will:

* Start two synthetic connectors
* Publish market data via `MarketDataBus`
* Run the strategy and supporting systems for approximately five seconds
* Stop all components and exit cleanly

## Notes

* This demo is intended for integration testing and illustration only
* Production deployments should define their own builder and execution harness
* All demo components are isolated and can be replaced with real implementations
