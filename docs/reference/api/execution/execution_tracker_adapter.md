# ExecutionTrackerAdapter

`ExecutionTrackerAdapter` is a proxy that forwards order execution events to an `IExecutionTracker`, timestamping each transition with `steady_clock::now()`.

```cpp
class ExecutionTrackerAdapter : public IOrderExecutionListener {
public:
  ExecutionTrackerAdapter(SubscriberId id, IExecutionTracker* tracker);
  // All IOrderExecutionListener methods overridden
private:
  IExecutionTracker* _tracker;
};
```

## Purpose

* Decouple execution tracking logic from the event delivery mechanism by timestamping and delegating events to a metrics recorder.

## Responsibilities

| Method                   | Delegated To                                 |
| ------------------------ | -------------------------------------------- |
| `onOrderSubmitted`       | `_tracker->onOrderSubmitted(..., now)`       |
| `onOrderAccepted`        | `_tracker->onOrderAccepted(..., now)`        |
| `onOrderPartiallyFilled` | `_tracker->onOrderPartiallyFilled(..., now)` |
| `onOrderFilled`          | `_tracker->onOrderFilled(..., now)`          |
| `onOrderCanceled`        | `_tracker->onOrderCanceled(..., now)`        |
| `onOrderExpired`         | `_tracker->onOrderExpired(..., now)`         |
| `onOrderRejected`        | `_tracker->onOrderRejected(..., now)`        |
| `onOrderReplaced`        | `_tracker->onOrderReplaced(..., now)`        |

## Notes

* Uses wall-clock `std::chrono::steady_clock::now()` to consistently timestamp all events.
* The adapter does not own the `IExecutionTracker`; it assumes external lifetime management.
* Enables clean separation of metrics collection from trading logic.
