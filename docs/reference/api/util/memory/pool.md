# Pool & Handle

This module implements a lock-free, reference-counted object pool for zero-allocation reuse of high-frequency data structures. It is optimized for HFT workloads with strict latency and memory control requirements.

## `pool::Pool<T, Capacity>`

A statically sized memory pool for pre-allocating `T` objects that conform to the `Poolable` concept.

```cpp
Pool<BookUpdateEvent, 8192> bookPool;
auto handle = bookPool.acquire(); // returns optional<Handle<T>>
```

### Purpose

* Eliminate runtime allocations in performance-critical paths.
* Efficiently recycle reusable objects like events or buffers.

### Responsibilities

| Feature      | Description                                                   |
| ------------ | ------------------------------------------------------------- |
| Allocation   | Constructs objects in-place using `std::pmr` memory resource. |
| Recycling    | Returns objects to the pool via `releaseToPool()`.            |
| Ref-counting | Uses intrusive reference counting (`retain`, `release`).      |
| Lifecycle    | Calls `clear()` and `resetRefCount()` on reuse.               |

## `pool::Handle<T>`

A move-only, reference-counted smart pointer for objects allocated from the pool.

```cpp
Handle<BookUpdateEvent> h = pool.acquire().value();
h->tickSequence = 123;
```

### Purpose

* Safely manage lifetime of pooled objects without heap allocations.

### Features

| Feature        | Description                                       |
| -------------- | ------------------------------------------------- |
| Move-only      | Copy retains reference; assignment is deleted.    |
| Auto-release   | Returns to pool when last reference is destroyed. |
| Type-safe cast | `upcast<U>()` supports safe widening conversions. |

## Type Requirements

`T` must:

* Inherit from `RefCountable` and `PoolableBase<T>`
* Implement:

  * `clear()`
  * `setPool(void*)`
  * `releaseToPool()`

## Internal Design

* `Pool<T>` uses `std::aligned_storage` for static placement.
* Objects are returned to the pool via an `SPSCQueue<T*>`.
* Backed by a `monotonic_buffer_resource` and `unsynchronized_pool_resource` for internal vector-like allocations.

## Exhaustion Handling

The pool provides callbacks and statistics for monitoring pool usage:

```cpp
pool.setExhaustionCallback([](size_t capacity, size_t inUse) {
  LOG_WARN("Pool exhausted: capacity={}, inUse={}", capacity, inUse);
});
```

| Method             | Description                                           |
| ------------------ | ----------------------------------------------------- |
| `capacity()`       | Returns the pool's maximum capacity.                  |
| `inUse()`          | Returns the number of currently acquired objects.     |
| `exhaustionCount()`| Returns how many times `acquire()` failed.            |
| `acquireCount()`   | Returns total number of successful acquisitions.      |
| `releaseCount()`   | Returns total number of releases back to pool.        |

The exhaustion callback is invoked each time `acquire()` returns `nullopt` due to pool exhaustion.

## Sizing Guidelines

When using pools with `EventBus`, the pool capacity **must be greater than** the EventBus capacity:

```cpp
// Correct: pool capacity (8191) > bus capacity (4096)
Pool<BookUpdateEvent, 8191> pool;
EventBus<Handle<BookUpdateEvent>, 4096> bus;

// Incorrect: will cause pool exhaustion
Pool<BookUpdateEvent, 4096> pool;  // Same as bus = will exhaust!
EventBus<Handle<BookUpdateEvent>, 4096> bus;
```

**Why?** EventBus only reclaims events when the ring buffer wraps around. If pool capacity ≤ bus capacity, all pool slots will be in-flight before any can be returned.

The default `config::DEFAULT_CONNECTOR_POOL_CAPACITY` (8191) is sized for this reason when used with `DEFAULT_EVENTBUS_CAPACITY` (4096).

## Notes

* Zero allocations in steady-state operation.
* Thread-safe for single-producer, single-consumer usage.
* All objects are destructed in-place on shutdown.
* Used extensively for `BookUpdateEvent`, `TradeEvent`, and other high-volume types.
