# SPSCQueue

`SPSCQueue` is a lock-free, bounded-size single-producer/single-consumer queue optimized for HFT workloads. It supports in-place construction, zero allocations, and cache-line isolation.

```cpp
template <typename T, size_t Capacity>
class SPSCQueue;
```

## Purpose

* Provide low-latency, zero-contention messaging between one writer and one reader.

## Requirements

* `Capacity` must be a power of two.
* `T` must be nothrow-destructible.
* Only one producer and one consumer may operate concurrently.

## Key Features

| Method               | Description                                                           |
| -------------------- | --------------------------------------------------------------------- |
| `push(const T&)`     | Enqueues a copy of an object.                                         |
| `emplace(T&&)`       | Enqueues an rvalue object (move).                                     |
| `try_emplace(...)`   | Constructs object in-place with arguments.                            |
| `pop(T&)`            | Pops and moves the front element into `out`.                          |
| `try_pop()`          | Returns a pointer to the front element, or `nullptr` if empty.        |
| `try_pop_ref()`      | Returns `std::optional<std::reference_wrapper<T>>` for inline access. |
| `empty()` / `full()` | Check queue state.                                                    |
| `clear()`            | Destroys and drains all pending elements.                             |
| `size()`             | Returns current number of elements.                                   |

## Internal Design

* Ring buffer implementation with `Capacity` entries, using modulo `MASK = Capacity - 1`.
* `_head` and `_tail` are `std::atomic<size_t>` and are false-shared-safe via `alignas(64)`.
* Uses placement `new` for in-place construction, avoids heap entirely.

## Notes

* Optimized for predictable, sub-microsecond latency in tight loops.
* No memory reclamation or ABA protection — not suitable for multi-producer/multi-consumer setups.
* All methods use `memory_order_acquire/release` to ensure visibility across cores.
* Destruction ensures safe draining of remaining elements via `~T()` call.
