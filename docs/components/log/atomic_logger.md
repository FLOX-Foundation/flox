# AtomicLogger

`AtomicLogger` is a low-latency, lock-free logger implementation designed for high-performance environments.  
It supports log-level filtering, auto-rotation by time or size, and writes to shared memory by default (`/dev/shm`).

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

| Field              | Description                                     |
| ------------------ | ----------------------------------------------- |
| `overflow`         | `Drop` or `Overwrite` when buffer is full       |
| `levelThreshold`   | Minimum `LogLevel` to log                       |
| `basename`         | Log file base name (e.g. `flox.log`)            |
| `directory`        | Directory for log output (e.g. `/dev/shm`)      |
| `maxFileSize`      | Maximum size before rotation                    |
| `rotateInterval`   | Time-based rotation window                      |
| `flushImmediately` | If `true`, flush immediately after each message |

## Implementation Details

* Ring buffer of fixed-size entries (1024)
* Each entry stores: timestamp, level, message (max 256 bytes)
* A background thread reads the buffer and writes to file
* Rotation occurs when file size exceeds `maxFileSize` or interval passes

## Threading Model

* **Writers**: lock-free, use atomic `_writeIndex`
* **Flusher**: single thread consumes entries using `_readIndex`
* **Coordination**: via condition variable (new entries or periodic wake-up)

## Sample Usage

```cpp
AtomicLogger logger;
logger.info("Engine started");
logger.warn("Price feed delayed");
logger.error("Order failed: rejected by risk");
```

## Format

Log entries are printed with timestamp and level prefix:

```
2025-07-14T08:42:03Z [INFO] Engine started
2025-07-14T08:42:04Z [WARN] Order queue near capacity
2025-07-14T08:42:05Z [ERROR] RiskManager::allow rejected order
```

## Notes

* Buffer overflow behavior depends on `OverflowPolicy`
* Log rotation renames the file with a timestamp suffix
* Avoid writing long messages: max message size is 256 bytes
* Log flushing is done in a separate thread to reduce latency

---

## Related

* [`ILogger`](./abstract_logger.md): base interface
* `OverflowPolicy`: defines handling strategy when buffer is full
* `LogLevel`: defines filtering threshold
