# ConsoleLogger

`ConsoleLogger` is a simple `ILogger` implementation that writes log messages directly to standard output.

## Purpose

- Provide a minimal, zero-setup logger for testing and development
- Print log messages to console (`stdout`)
- Filter messages based on severity level

## Construction

```cpp
ConsoleLogger logger(LogLevel::Warn);
logger.info("This will be filtered out");
logger.warn("This will be printed");
```

The constructor accepts a `LogLevel` threshold. Messages below the threshold are ignored.

## Methods

| Method            | Description                                    |
| ----------------- | ---------------------------------------------- |
| `info(msg)`       | Logs a message with `LogLevel::Info`           |
| `warn(msg)`       | Logs a message with `LogLevel::Warn`           |
| `error(msg)`      | Logs a message with `LogLevel::Error`          |
| `log(level, msg)` | Logs a message at specified level (manual use) |

## Output Format

The logger prepends each message with a fixed label, e.g.:

```
[INFO]  Engine started
[WARN]  Config missing field 'symbol'
[ERROR] Order rejected by validator
```

## Filtering

Only messages with a level **greater than or equal to** the configured `LogLevel` are printed:

```cpp
ConsoleLogger logger(LogLevel::Warn);
logger.info("Ignored");
logger.warn("Printed");
```

## Notes

* `ConsoleLogger` is not thread-safe
* Suitable only for testing, not recommended for production
* Use `AtomicLogger` for performance-critical logging

## Related

* [`ILogger`](./abstract_logger.md): base interface
* `LogLevel`: severity levels used for filtering
