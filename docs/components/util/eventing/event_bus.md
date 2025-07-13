# EventBus

`EventBus` is a lock-free fan-out messaging system for publishing typed events to multiple subscribers. It supports both **push** (threaded delivery) and **pull** (manual draining) modes, and is configurable for **sync** or **async** semantics via policy injection.

```cpp
template <typename Event, typename Policy, size_t QueueSize = config::DEFAULT_EVENTBUS_QUEUE_SIZE>
class EventBus;
```

## Purpose

* Deliver high-frequency events (market data, orders, etc.) to multiple subscribers with minimal latency and zero allocations.

## Supported Policies

| Policy        | Description                                                                |
| ------------- | -------------------------------------------------------------------------- |
| `AsyncPolicy` | Lock-free delivery in per-subscriber threads with no tick synchronization. |
| `SyncPolicy`  | Uses `TickBarrier` to coordinate delivery across all subscribers per tick. |

## Key Responsibilities

| Method                | Description                                                             |
| --------------------- | ----------------------------------------------------------------------- |
| `subscribe()`         | Registers a new subscriber with individual queue and mode.              |
| `publish()`           | Broadcasts an event to all subscribers; sync-mode waits for completion. |
| `start()` / `stop()`  | Starts or stops all push-mode subscriber threads.                       |
| `getQueue(id)`        | Provides direct access to a subscriber’s queue (for pull-mode).         |
| `enableDrainOnStop()` | Ensures any remaining events are dispatched before shutdown.            |

## Design Highlights

* **Queue per subscriber**: Each listener has a dedicated `SPSCQueue` to avoid contention.
* **Thread-per-subscriber**: Only for `PUSH` mode; `PULL` consumers must drain manually.
* **RAII-controlled sync**: `TickGuard` ensures deterministic behavior in `SyncPolicy`.
* **Zero dynamic allocations** in hot path (`emplace()` used directly).
* **Tick-sequenced events**: `tickSequence` field is automatically set if present.

## Internal Types

| Name        | Description                                                         |
| ----------- | ------------------------------------------------------------------- |
| `QueueItem` | Depends on `Policy`: either raw `Event` or `(Event, TickBarrier*)`. |
| `Listener`  | Inferred from `Event::Listener`.                                    |
| `Queue`     | Lock-free ring buffer (single producer/consumer).                   |
| `Entry`     | Subscription record: listener, queue, thread, and mode.             |

## Notes

* Sync-mode (`SyncPolicy`) is ideal for deterministic backtesting or simulation.
* Async-mode (`AsyncPolicy`) is best suited for ultra-low-latency live systems.
* `EventBus` is fully generic — works with any event type that defines a `Listener`.

## Example Usage

```cpp
using BookBus = EventBus<pool::Handle<BookUpdateEvent>, AsyncPolicy<pool::Handle<BookUpdateEvent>>>;
BookBus bus;
bus.subscribe(bookHandler);
bus.publish(bookUpdateHandle);
```
