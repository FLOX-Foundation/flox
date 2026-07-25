# RefCountable

`RefCountable` is a low-overhead, intrusive reference counting base class. It enables manual control of object lifetime without dynamic memory management and is used as the foundation for pooled, shared objects in FLOX.

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
| `retain()`        | Atomic `fetch_add(1, relaxed)` on the reference count.  |
| `release()`       | Atomic `fetch_sub(1, acq_rel)`; returns `true` if this was the last ref. |
| `resetRefCount()` | Resets ref count to 0 or specified value.               |
| `refCount()`      | Returns current ref count for debug/inspection.         |

## Behavior

* When `release()` returns `true`, the object is no longer in use and may be recycled.
* Incorrect calls (e.g. `release()` on `0`) are fatal in debug builds and abort in release.

## Design Notes

* The counter is `std::atomic<uint32_t>`. Orderings differ per operation: `retain()`, `resetRefCount()` and `refCount()` use `relaxed`; `release()` uses `acq_rel` so the decrement that reaches zero synchronizes with the prior releases.
* Every method is `noexcept`.
* Thread-safe under the assumption that retain/release are called from valid ownership contexts.
* Not designed for multi-owner concurrent access — intended for single-threaded or externally synchronized lifecycles.

## Concept

The concept lives in `flox::concepts`, not at namespace scope, so it does not collide with the
`flox::RefCountable` class. Spell it `flox::concepts::RefCountable`.

```cpp
namespace concepts
{
template <typename T>
concept RefCountable = requires(T obj) {
  { obj.retain() } -> std::same_as<void>;
  { obj.release() } -> std::same_as<bool>;
  { obj.resetRefCount() } -> std::same_as<void>;
};
}  // namespace concepts
```

This concept ensures compile-time validation for use in pooled or handle-managed objects.
