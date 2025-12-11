# MultiExecutionListener

`MultiExecutionListener` fans out order lifecycle events to multiple `IOrderExecutionListener` targets. It acts as a multicast adapter for `OrderExecutionBus` subscribers.

```cpp
class MultiExecutionListener : public IOrderExecutionListener {
public:
  explicit MultiExecutionListener(SubscriberId id);
  void addListener(IOrderExecutionListener* listener);

  // All event methods delegate to added listeners
};
```

## Purpose

* Aggregate multiple execution listeners into a single subscriber to avoid duplicating subscriptions to the `OrderExecutionBus`.

## Responsibilities

| Method          | Behavior                                         |
| --------------- | ------------------------------------------------ |
| `addListener()` | Registers a new listener (if not already added). |
| Event handlers  | Forwards all events to each registered listener. |

## Notes

* Listeners are stored as raw pointers; ownership is not transferred.
* Prevents duplicate listeners using `std::ranges::find()`.
* Efficient for use cases like simultaneously tracking PnL, latency, and order audit logs.
