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
