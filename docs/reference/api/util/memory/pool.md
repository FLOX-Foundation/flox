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

## Notes

* Zero allocations in steady-state operation.
* Thread-safe for single-producer, single-consumer usage.
* All objects are destructed in-place on shutdown.
* Used extensively for `BookUpdateEvent`, `TradeEvent`, and other high-volume types.
