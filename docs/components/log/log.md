# Logging Macros

Flox provides a set of lightweight logging macros with compile-time and runtime control. These macros wrap `LogStream` for structured message building and support selective log-level filtering.

## Macros Overview

| Macro               | Description                                      |
|---------------------|--------------------------------------------------|
| `FLOX_LOG(...)`     | Logs message at `Info` level                     |
| `FLOX_LOG_INFO(...)`| Shortcut for `Info` level                        |
| `FLOX_LOG_WARN(...)`| Shortcut for `Warn` level                        |
| `FLOX_LOG_ERROR(...)`| Shortcut for `Error` level                      |
| `FLOX_LOG_LEVEL(lvl, ...)` | Logs at custom level (`LogLevel`)        |
| `FLOX_LOG_ON()`     | Enable runtime logging                          |
| `FLOX_LOG_OFF()`    | Disable runtime logging                         |

## LogStream Integration

All macros internally use `LogStream`, a RAII-style helper that sends the log message when it goes out of scope.

Example:

```cpp
FLOX_LOG("Book update: bid=" << update.bestBidPrice());
FLOX_LOG_WARN("Rejecting order due to risk check failure");
```

This is equivalent to:

```cpp
if (isLoggingEnabled())
  LogStream(LogLevel::Warn) << "Rejecting order due to risk check failure";
```

The benefit is a clean, familiar `operator<<` syntax and delayed message formatting, only evaluated if logging is enabled.

## Compile-Time Disable

If `FLOX_DISABLE_LOGGING` is defined at compile time, all logging macros become no-ops:

```cpp
#define FLOX_DISABLE_LOGGING
```

This is useful for benchmark builds or environments where logging must be completely stripped out.

## Thread Safety

* Logging macros are thread-safe if the selected logger (e.g. `AtomicLogger`) is thread-safe.
* Overhead is minimal: each macro checks a global atomic `loggingEnabled` flag before constructing a `LogStream`.

## Notes

* No log message will be emitted if `FLOX_LOG_OFF()` was called or logging was disabled at runtime.
* Message formatting is deferred until `LogStream` destructor runs.
* Logs can be redirected by configuring a global logger (`ILogger` implementation).
