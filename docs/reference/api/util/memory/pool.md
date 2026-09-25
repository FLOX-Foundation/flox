# Pool & Handle

Lock-free, reference-counted object pool for reusing fixed-size objects without heap allocation.

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
| Lifecycle    | Calls `clear()` and `resetRefCount()` on reuse; `~Pool()` destroys every slot exactly once. |

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
  * `setPool(pool::PoolReleaser*)`
  * `releaseToPool()`

## Internal Design

* `Pool<T>` stores slots in a hand-rolled `struct alignas(alignof(T)) Storage { std::byte data[sizeof(T)]; }` array for static placement. `std::aligned_storage` is deprecated and is not used.
* Objects are returned to the pool through a lock-free index freelist (`IndexFreelist`), which accepts `push` and `pop` from any thread. This is not a convenience: a bus slot owns its `Handle` until the slot is overwritten, and the overwrite runs on whichever thread is publishing, so with several connectors sharing a bus an event returns to its pool from a foreign thread. The freelist stores 32-bit slot indices with an ABA tag packed into one 64-bit word.
* Each object holds a pointer to the pool that owns its slot, reached through the type-erased `pool::PoolReleaser` interface that `Pool` implements. A process commonly runs several pools of the same `T`, one per connector, so the owner has to be a property of the object rather than of its type. An object returns to the pool its slot belongs to, and destroying one pool leaves every other pool of that type alone.
* Each slot carries a claim flag saying whether it is currently handed out. A release of an object that is not claimed — a stale `Handle` copy, a bus slot destroyed twice, a connector releasing what it already published — is refused and counted in `invalidReleaseCount()` rather than pushing the same index onto the freelist twice. The pool cannot hand one slot to two acquirers, and it does not abort a trading process over a caller's mistake.
* `inUse()` is one counter, not `acquireCount() - releaseCount()`. The difference of two independently-sampled counters underflowed to a number near 2^64 when a release landed between the two loads, and that value was what the exhaustion callback was handed.
* `~Pool()` runs `~T()` over every slot. The constructor placement-news one object per slot, so anything a pooled type holds outside the pmr arena — a `std::string`'s buffer, a `shared_ptr`, a descriptor — is released here; a defaulted destructor leaked one per slot for the life of the process.
* `acquireCount()` / `releaseCount()` / `exhaustionCount()` / `invalidReleaseCount()` are atomic and safe to read while the pool is in use.
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
| `invalidReleaseCount()` | Returns how many releases were refused (double release, foreign or null pointer). |

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
* Acquire and release are safe from any thread; release in particular runs on whichever thread drops the last reference.
* All objects are destructed in-place on shutdown, including any still acquired.
* Used extensively for `BookUpdateEvent`, `TradeEvent`, and other high-volume types.
