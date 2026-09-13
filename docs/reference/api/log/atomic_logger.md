# AtomicLogger

`AtomicLogger` is a lock-free logger. Supports log-level filtering, auto-rotation by time or size, writes to shared memory by default (`/dev/shm`).

## Purpose

- Avoid allocations and locks on the hot path (`info`, `warn`, `error`)
- Defer I/O to a background thread
- Support size/time-based log rotation
- Enable high-throughput logging in performance-critical systems

## Construction

```cpp
AtomicLoggerOptions opts;
opts.levelThreshold = LogLevel::Warn;
opts.basename = "flox.log";
opts.directory = "/var/log/flox";
opts.maxFileSize = 10 * 1024 * 1024;
opts.rotateInterval = std::chrono::minutes(30);

auto logger = std::make_unique<AtomicLogger>(opts);
```

## Options

| Field              | Default              | Description                                     |
| ------------------ | -------------------- | ----------------------------------------------- |
| `overflow`         | `Drop`               | `Drop` or `Overwrite` when buffer is full       |
| `levelThreshold`   | `LogLevel::Info`     | Minimum `LogLevel` to log                       |
| `basename`         | `"flox.log"`         | Log file base name                              |
| `directory`        | `"/dev/shm"`         | Directory for log output                        |
| `maxFileSize`      | 100 MB               | Maximum size before rotation                    |
| `rotateInterval`   | 60 minutes           | Time-based rotation window                      |
| `flushImmediately` | `true`               | If `true`, flush immediately after each message |

The example above overrides several of these; a default-constructed
`AtomicLoggerOptions` rotates at 100 MB or 60 minutes, not at the example's 10 MB / 30 minutes.

## Methods

| Method | Description |
|--------|-------------|
| `info(msg)` / `warn(msg)` / `error(msg)` | `ILogger` overrides. Lock-free on the caller's thread |
| `flush()` | Drain the ring buffer to the file synchronously. Public; call it before shutdown or when you need the file to be current. Returns once every message claimed before the call has been written out and the file has been flushed |

## Implementation Details

* Ring buffer of fixed-size entries (1024)
* Each entry stores: timestamp, level, message (max 256 bytes)
* A background thread reads the buffer and writes to file
* Rotation occurs when file size exceeds `maxFileSize` or interval passes

## Threading Model

* **Writers**: lock-free. A writer claims a position by advancing an atomic
  index, fills the entry, then publishes it.
* **Flusher**: one background thread. It writes a published entry out and only
  then hands the slot back to the writers.
* **Slot state**: each slot carries its own ring position, so a writer can
  claim it only after the flusher has finished with it. A writer cannot take
  the slot the flusher is reading, and the flusher cannot clear a publication
  that belongs to the next lap of the ring.
* **Overflow**: a full ring means every slot still holds a message the flusher
  has not written out. `Drop` discards the new message. `Overwrite` retires the
  oldest message to make room, and drops the new one instead if another writer
  is still filling the oldest.
* **Wake-up**: a condition variable, signalled on a new entry and on a 1 ms
  timer.
* **File ownership**: the `FILE*` belongs to the flush thread alone, because
  that thread is the one that rotates, which closes the handle and opens
  another. `flush()` runs on the caller's thread, so it does not touch the
  handle: it waits for the drain, raises a request, and waits for the flush
  thread to acknowledge it. Reading the handle from the caller meant a
  rotation landing mid-call left the caller flushing a descriptor that had
  just been closed.

## Sample Usage

```cpp
AtomicLogger logger;
logger.info("Engine started");
logger.warn("Price feed delayed");
logger.error("Order failed: rejected by risk");
```

## Format

Log entries carry a local timestamp with milliseconds, then the level:

```
[20250714-084203.118] INFO: Engine started
[20250714-084204.902] WARN: Order queue near capacity
[20250714-084205.331] ERROR: RiskManager::allow rejected order
```

## Rotation

The current file is always `<directory>/<basename>`. On rotation it is renamed
to `<basename>.<YYYYMMDD-HHMMSS.mmm>.log`; if that name is taken, a `-1`, `-2`
suffix is appended until one is free. Size-based rotation fires many times a
second on a busy logger, which is why the name carries milliseconds and why the
suffix exists at all. Without them the second rename of a given second silently
replaces the first archive.

## Notes

* Buffer overflow behavior depends on `OverflowPolicy`
* Avoid writing long messages: max message size is 256 bytes
* Log flushing is done in a separate thread to reduce latency

---

## Related

* [`ILogger`](./abstract_logger.md): base interface
* `OverflowPolicy`: defines handling strategy when buffer is full
* `LogLevel`: defines filtering threshold
