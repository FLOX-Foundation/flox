# IExecutionTracker

`IExecutionTracker` defines an interface for tracking order lifecycle events with precise timestamps. It is typically used for latency analysis, logging, and performance metrics.

```cpp
class IExecutionTracker : public ISubsystem {
public:
  virtual ~IExecutionTracker() = default;

  virtual void onOrderSubmitted(const Order& order, TimePoint ts) = 0;
  virtual void onOrderAccepted(const Order& order, TimePoint ts) = 0;
  virtual void onOrderPartiallyFilled(const Order& order, Quantity fillQty, TimePoint ts) = 0;
  virtual void onOrderFilled(const Order& order, TimePoint ts) = 0;
  virtual void onOrderCanceled(const Order& order, TimePoint ts) = 0;
  virtual void onOrderExpired(const Order& order, TimePoint ts) = 0;
  virtual void onOrderRejected(const Order& order, const std::string& reason, TimePoint ts) = 0;
  virtual void onOrderReplaced(const Order& oldOrder, const Order& newOrder, TimePoint ts) = 0;
};
```

## Purpose

* Capture precise timing of each order state transition for performance diagnostics and post-trade analysis.

## Responsibilities

| Method                   | Captures                                                       |
| ------------------------ | -------------------------------------------------------------- |
| `onOrderSubmitted`       | Time of initial submission.                                    |
| `onOrderAccepted`        | Time acknowledged by exchange or venue.                        |
| `onOrderPartiallyFilled` | Time of partial fill, including filled quantity.               |
| `onOrderFilled`          | Time of full fill.                                             |
| `onOrderCanceled`        | Time of cancel confirmation.                                   |
| `onOrderExpired`         | Time the order was marked as expired.                          |
| `onOrderRejected`        | Time of rejection, including optional reason.                  |
| `onOrderReplaced`        | Time of order replacement, with reference to both old and new. |

## Notes

* All timestamps are provided externally (usually by `ExecutionTrackerAdapter`) to ensure consistency.
* Used in simulations and live systems for detailed latency tracking and event sequencing.
* Inherits from `ISubsystem` for lifecycle integration with the engine.
