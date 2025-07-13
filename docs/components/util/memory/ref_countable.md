# RefCountable

`RefCountable` is a low-overhead, intrusive reference counting base class. It enables manual control of object lifetime without dynamic memory management and is used as the foundation for pooled, shared objects in Flox.

```cpp
class RefCountable {
public:
  void retain() noexcept;
  bool release() noexcept;
  void resetRefCount(uint32_t value = 0) noexcept;
  uint32_t refCount() const noexcept;
};
```

## Purpose

* Provide deterministic, allocation-free lifetime tracking for objects managed in pools or event buses.

## Responsibilities

| Method            | Description                                             |
| ----------------- | ------------------------------------------------------- |
| `retain()`        | Increments reference count (non-atomic relaxed).        |
| `release()`       | Decrements reference count; returns `true` if last ref. |
| `resetRefCount()` | Resets ref count to 0 or specified value.               |
| `refCount()`      | Returns current ref count for debug/inspection.         |

## Behavior

* When `release()` returns `true`, the object is no longer in use and may be recycled.
* Incorrect calls (e.g. `release()` on `0`) are fatal in debug builds and abort in release.

## Design Notes

* Uses `std::atomic<uint32_t>` with relaxed memory ordering for performance.
* Thread-safe under the assumption that retain/release are called from valid ownership contexts.
* Not designed for multi-owner concurrent access — intended for single-threaded or externally synchronized lifecycles.

## Concept

```cpp
template <typename T>
concept RefCountable = requires(T obj) {
  { obj.retain() } -> std::same_as<void>;
  { obj.release() } -> std::same_as<bool>;
  { obj.resetRefCount() } -> std::same_as<void>;
};
```

This concept ensures compile-time validation for use in pooled or handle-managed objects.
