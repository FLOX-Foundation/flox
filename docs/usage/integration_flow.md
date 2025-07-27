## Flox System Integration Flow (Overview)

### 1. **Register Symbols**

Before anything else, the system must define which trading instruments it will operate on. Each symbol is registered with an internal registry, which assigns it a unique identifier and stores metadata (such as exchange name, symbol string, and instrument type).

### 2. **Instantiate Core Components**

Several core components must be created and wired together:

* A **symbol registry** to resolve and map symbols across the system.
* An **order tracker** to monitor local orders and maintain their current state.
* **Event buses** for market data and order events (e.g., for book updates, trades, and execution events).

These components typically live for the entire lifetime of the system.

### 3. **Create Executors and Connectors**

* An **order executor** is responsible for sending orders to the exchange and reporting their status.
* An **exchange connector** connects to the market (e.g., via WebSocket or REST), receives real-time data, and publishes it to the system.

Both components should be provided with access to the registry, tracker, and transport layer.

### 4. **Set Up Event Delivery**

Event buses are configured to distribute market data (book updates, trades) and internal events (such as order execution updates) to multiple subscribers.

Subscribers may include:

* Trading strategies
* Data aggregators
* Metric collectors
* Risk modules

Each subscriber declares its interest and is registered with the corresponding bus.

### 5. **Launch Components**

Once everything is wired:

* Event buses are started to begin handling data.
* Exchange connectors establish connections and begin streaming data.
* Strategies are started and begin processing events and generating signals.
* Executors handle outgoing order flow and interact with the tracker and listener components.

### 6. **Handle Execution Feedback**

When orders are submitted, filled, rejected, or canceled, the system updates the order tracker and may notify listeners or emit events on an execution bus.

This ensures consistency of order state and provides visibility into execution outcomes.

### 7. **Shutdown Procedure**

When shutting down:

* Event buses are stopped to cease fan-out.
* Strategies and connectors are stopped gracefully.
* Executors complete any in-flight work and clean up.

## Notes for Implementers

* The **registry and tracker** must be passed to any component that deals with symbols or order state.
* **Thread safety** and **latency guarantees** are core to the design — use lock-free or atomic constructs where applicable.
* **No dynamic allocation** in critical paths (e.g., during fan-out or order submission).
* Buses can operate in **sync** or **async** mode depending on requirements (e.g., determinism vs performance).
* The architecture is **modular and decoupled** — new strategies, connectors, or execution backends can be plugged in easily.
