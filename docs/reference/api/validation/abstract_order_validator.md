# IOrderValidator

`IOrderValidator` defines the interface for validating outbound orders before submission. It ensures correctness and compliance with predefined constraints (e.g. price ranges, order size).

```cpp
class IOrderValidator : public ISubsystem {
public:
  virtual ~IOrderValidator() = default;
  virtual bool validate(const Order& order, std::string& reason) const = 0;
};
```

## Purpose

* Prevent invalid or unsafe orders from reaching the execution layer by performing sanity checks.

## Responsibilities

| Method     | Description                                                                |
| ---------- | -------------------------------------------------------------------------- |
| `validate` | Checks if the order is valid. Returns `true` if valid; else sets `reason`. |

## Notes

* Must be called prior to invoking `IOrderExecutor::submitOrder()`.
* Provides human-readable error messages via the `reason` output parameter.
* Implementations can enforce checks such as:

  * Non-zero quantity
  * Price within expected deviation
  * Tick-size alignment
  * Symbol validity
* Integrated into the engine as an `ISubsystem` for lifecycle coordination.
