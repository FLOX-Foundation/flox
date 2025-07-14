# Logger

The `ILogger` interface defines a minimal, abstract logging API for internal and external use.

It provides a consistent way to report messages of varying severity and supports multiple logging strategies, including console, file, and shared-memory loggers.

## Interface

```cpp
enum class LogLevel {
  Info,
  Warn,
  Error
};

enum class OverflowPolicy {
  Drop,      // Silently drop new messages when buffer is full
  Overwrite  // Overwrite oldest messages
};

struct ILogger {
  virtual ~ILogger() = default;

  virtual void info(std::string_view msg) = 0;
  virtual void warn(std::string_view msg) = 0;
  virtual void error(std::string_view msg) = 0;
};
```

## Usage

You can implement `ILogger` to customize how logs are handled. For example:

* Writing to `stdout` or `stderr`
* Writing to rotating log files
* Logging to `/dev/shm` for high-speed shared memory logging
* Filtering messages based on `LogLevel`
* Batching or compressing logs for network transmission

Example implementation for stdout:

```cpp
struct StdoutLogger : public ILogger {
  void info(std::string_view msg) override {
    std::cout << "[INFO] " << msg << std::endl;
  }
  void warn(std::string_view msg) override {
    std::cout << "[WARN] " << msg << std::endl;
  }
  void error(std::string_view msg) override {
    std::cerr << "[ERROR] " << msg << std::endl;
  }
};
```

## Overflow Policy

When used in conjunction with buffered or lock-free logging systems, `OverflowPolicy` governs what happens when the log buffer is full:

* `Drop`: new incoming messages are discarded.
* `Overwrite`: older messages are overwritten to make space.

This allows you to trade off between completeness and real-time guarantees.

## Best Practices

* Do not use logging in latency-critical paths (e.g. market data callbacks) unless the logger is designed for low-latency (e.g. lock-free).
* Prefer shared-memory or file-backed logging for persistency.
* Use `LogLevel` filtering to avoid excessive log volume in production.

## Related

* `AtomicLogger`: ultra-low-latency lock-free logger implementation in Flox
* `LogLevel`: enumeration for severity control
* `OverflowPolicy`: controls log buffering behavior
