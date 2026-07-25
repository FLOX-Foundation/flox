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
| `flush()` | Drain the ring buffer to the file synchronously. Public; call it before shutdown or when you need the file to be current |

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
