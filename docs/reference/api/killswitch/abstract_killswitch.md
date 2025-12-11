# IKillSwitch

`IKillSwitch` defines the interface for components that enforce emergency shutdown conditions in the trading system, typically in response to loss limits, excessive order flow, or abnormal conditions.

```cpp
class IKillSwitch : public ISubsystem {
public:
  virtual ~IKillSwitch() = default;

  virtual void check(const Order& order) = 0;
  virtual void trigger(const std::string& reason) = 0;
  virtual bool isTriggered() const = 0;
  virtual std::string reason() const = 0;
};
```

## Purpose

* Provide runtime protection against runaway strategies, loss accumulation, or systemic failures by halting all trading activity.

## Responsibilities

| Method          | Description                                              |
| --------------- | -------------------------------------------------------- |
| `check(order)`  | Evaluates the incoming order against risk limits.        |
| `trigger()`     | Manually activates the kill switch with a reason string. |
| `isTriggered()` | Returns whether the kill switch is currently active.     |
| `reason()`      | Returns the human-readable cause of activation.          |

## Notes

* Implements `ISubsystem`, allowing coordinated startup and reset across engine lifecycle.
* Can be wired into strategy layer, order executor, or global event loop.
* Once triggered, downstream components are expected to halt order submission and processing.
