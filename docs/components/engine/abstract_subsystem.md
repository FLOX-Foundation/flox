# ISubsystem

`ISubsystem` defines a lifecycle interface for engine components that require explicit startup and shutdown phases.

```cpp
class ISubsystem
{
public:
  virtual ~ISubsystem() = default;

  virtual void start() {}
  virtual void stop() {}
};
```

## Purpose

* Provide deterministic initialization and teardown hooks for stateful modules in the system.

## Responsibilities

| Method | Description                         |
| ------ | ----------------------------------- |
| start  | Called during engine bootstrapping. |
| stop   | Called during shutdown or reset.    |

## Notes

* Methods have default empty implementations, allowing derived classes to override only what they need.
* Used by core modules like `CandleAggregator`, `Strategy`, `ExecutionTracker`, `SymbolRegistry`, etc.
* Lifecycle is typically orchestrated by the engine or test harness.
* No assumptions about threading — start/stop are always externally coordinated.

---

# IDrainable

`IDrainable` is a separate interface for components with pending async work that must complete before shutdown.

```cpp
class IDrainable
{
public:
  virtual ~IDrainable() = default;
  virtual bool drain(uint32_t timeoutMs) = 0;
};
```

## Purpose

* Wait for in-flight operations (e.g., pending orders, network requests) to complete.

## When to Implement

Only implement `IDrainable` for components with actual async work:
* Exchange connectors (in-flight order confirmations)
* Order executors (pending order submissions)
* Network transports (pending requests)

Do NOT implement for stateless or synchronous components like strategies, aggregators, or registries.
