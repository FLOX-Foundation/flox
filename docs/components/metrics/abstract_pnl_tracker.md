# IPnLTracker

`IPnLTracker` defines a minimal interface for tracking realized and unrealized profit and loss (PnL) based on filled orders.

```cpp
class IPnLTracker : public ISubsystem {
public:
  virtual ~IPnLTracker() = default;
  virtual void onOrderFilled(const Order& order) = 0;
};
```

## Purpose

* Record and update PnL metrics in response to order fill events.

## Responsibilities

| Method          | Description                                        |
| --------------- | -------------------------------------------------- |
| `onOrderFilled` | Called when an order is fully filled; updates PnL. |

## Notes

* Invoked only on complete fills — partial fills should be handled at a higher level if needed.
* Used in both real-time and simulation modes to compute performance metrics.
* Inherits from `ISubsystem` for coordinated lifecycle management.
