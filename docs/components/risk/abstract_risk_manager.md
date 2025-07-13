# IRiskManager

`IRiskManager` defines an interface for validating whether a given order complies with system-defined risk constraints before it is submitted.

```cpp
class IRiskManager : public ISubsystem {
public:
  virtual ~IRiskManager() = default;
  virtual bool allow(const Order& order) const = 0;
};
```

## Purpose

* Enforce pre-trade risk checks such as size limits, leverage rules, or order throttling.

## Responsibilities

| Method    | Description                                          |
| --------- | ---------------------------------------------------- |
| `allow()` | Returns `true` if the order is permitted to proceed. |

## Notes

* Called by strategy or execution layer before submitting an order.
* Stateless implementations may rely solely on order parameters; stateful variants may track recent flow.
* Integrated into the engine via `ISubsystem` for unified startup and reset behavior.
