# SPSCQueue

`SPSCQueue` is a lock-free, bounded single-producer/single-consumer queue. Supports in-place construction and cache-line isolation.

```cpp
template <typename T, size_t Capacity>
class SPSCQueue;
```

## Purpose

* Provide low-latency, zero-contention messaging between one writer and one reader.

## Requirements

Enforced by `static_assert`:

* `Capacity > 1`.
* `Capacity` must be a power of two.
* `T` must be nothrow-destructible.
* `T` must be nothrow-move-constructible.

Not enforced by the compiler:

* Only one producer and one consumer may operate concurrently.

## Key Features

| Method               | Description                                                           |
| -------------------- | --------------------------------------------------------------------- |
| `push(const T&)`     | Enqueues a copy of an object.                                         |
| `emplace(T&&)`       | Enqueues an rvalue object (move).                                     |
| `try_emplace(...)`   | Constructs object in-place with arguments.                            |
| `pop(T&)`            | Pops and moves the front element into `out`.                          |
| `try_pop()`          | Returns a borrowed pointer to the front element, or `nullptr` if empty. See [Borrowed pointers](#borrowed-pointers). |
| `try_pop_ref()`      | Returns `std::optional<std::reference_wrapper<T>>` for inline access. Same borrowing rules. |
| `empty()` / `full()` | Check queue state.                                                    |
| `clear()`            | Destroys and drains all pending elements.                             |
| `size()`             | Returns current number of elements.                                   |

## Borrowed pointers

`try_pop()` and `try_pop_ref()` hand back a pointer into the ring and republish
the slot to the producer immediately. The pointer is borrowed, not owned. It is
valid until the consumer's next call that moves the tail (`pop`, `try_pop`,
`try_pop_ref`, `clear`), and the caller destroys the element through it before
that point.

With exactly one pointer outstanding the ring invariant protects it. One slot
is always left empty, so a producer filling the queue stops one short of the
slot just handed out. Hold two at once and that protection is gone: the
producer can construct a new element into the first slot, the stale pointer
then reads the new element instead of the old one, and destroying it through
that pointer destroys an element still queued for delivery. The next `pop`
reads the wreckage.

To consume several elements at a time, use the segment API instead. It hands
out a contiguous run and holds the slots until `commit_read`:

```cpp
T* items = nullptr;
const size_t n = q.read_segment(items);
for (size_t i = 0; i < n; ++i) { process(items[i]); items[i].~T(); }
q.commit_read(n);
```

## Internal Design

* Ring buffer implementation with `Capacity` entries, using modulo `MASK = Capacity - 1`.
* `_head` and `_tail` are `std::atomic<size_t>` and are false-shared-safe via `alignas(64)`.
* Uses placement `new` for in-place construction, avoids heap entirely.

## Notes

* Optimized for predictable, sub-microsecond latency in tight loops.
* No memory reclamation or ABA protection — not suitable for multi-producer/multi-consumer setups.
* All methods use `memory_order_acquire/release` to ensure visibility across cores.
* Destruction ensures safe draining of remaining elements via `~T()` call.
