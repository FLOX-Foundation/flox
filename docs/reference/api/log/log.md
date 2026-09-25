# Logging Macros

FLOX provides a set of lightweight logging macros with compile-time and runtime control. These macros wrap `LogStream` for structured message building and support selective log-level filtering.

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
if (isLoggingEnabled() && LogLevel::Warn >= logLevel())
  LogStream(LogLevel::Warn) << "Rejecting order due to risk check failure";
```

The benefit is a clean, familiar `operator<<` syntax and message formatting
that does not happen at all unless the line is going to be written.

## The level is checked before anything is built

`logLevel()` is the threshold of the installed sink, republished by
`setGlobalLogger()` so the macro reads one atomic instead of a pointer plus a
virtual call. A line below it costs the comparison and nothing else: no
`LogStream`, no `std::ostringstream`, and none of the arguments streamed into
it are evaluated.

```cpp
ConsoleLogger errorsOnly(LogLevel::Error);
setGlobalLogger(&errorsOnly);

FLOX_LOG_INFO("book depth " << summariseBook(book));  // summariseBook is not called
```

The threshold used to live inside the sink alone, applied after the allocation
and after every argument had been formatted. `FLOX_LOG_WARN` sits on
`OrderTracker`'s unknown-order path and in the EventBus consumer loop, so a
reconnect burst was an allocation storm on the execution path for messages
nobody would read.

A sink that does not declare a threshold accepts everything: `ILogger::minLevel()`
returns `LogLevel::Info` by default. A sink that filters further internally is
free to do so — the macro's check is the cheap one, not the only one.

## Compile-Time Disable

If `FLOX_DISABLE_LOGGING` is defined at compile time, all logging macros become no-ops:

```cpp
#define FLOX_DISABLE_LOGGING
```

This is useful for benchmark builds or environments where logging must be completely stripped out.

## Thread Safety

* Logging macros are thread-safe if the selected logger (e.g. `AtomicLogger`) is thread-safe.
* Overhead is minimal: each macro checks the global atomic `loggingEnabled` flag and the global level before constructing a `LogStream`.

## Notes

* No log message will be emitted if `FLOX_LOG_OFF()` was called, logging was disabled at runtime, or the level is below the installed sink's `minLevel()`.
* Message formatting is deferred until `LogStream` destructor runs, and does not happen at all for a filtered line.
* Logs can be redirected by configuring a global logger (`ILogger` implementation).
