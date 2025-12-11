# IStorageSink

`IStorageSink` defines the interface for persisting executed orders and related trading data. It abstracts away the underlying storage mechanism (e.g. MongoDB, binary log, cloud).

```cpp
class IStorageSink : public ISubsystem {
public:
  virtual ~IStorageSink() = default;
  virtual void store(const Order& order) = 0;
};
```

## Purpose

* Persist orders for post-trade audit, reconciliation, analytics, or compliance.

## Responsibilities

| Method  | Description                           |
| ------- | ------------------------------------- |
| `store` | Persists the provided `Order` object. |

## Notes

* Called when orders are filled, canceled, or otherwise finalized.
* Backends may include MongoDB, file-based logs, or in-memory mirrors.
* Integrated via `ISubsystem` for lifecycle control and flush handling.
