# RateLimiter

Thread-safe token bucket rate limiter for controlling request/operation frequency.

## Quick Start

```cpp
#include "flox/util/rate_limiter.h"

// 10 requests per second
RateLimiter limiter({.capacity = 10, .refillRate = 10});

if (limiter.tryAcquire()) {
  sendRequest();
} else {
  // Rate limit exceeded
}
```

## Use Cases

- **API rate limiting** — prevent exchange bans
- **Log throttling** — avoid log spam
- **Alert throttling** — limit notification frequency
- **Order frequency** — cap orders per second

## API

### Constructor

```cpp
RateLimiter(Config config);

struct Config {
  uint32_t capacity;    // Maximum tokens in bucket
  uint32_t refillRate;  // Tokens added per second
};
```

### Methods

| Method | Description |
|--------|-------------|
| `tryAcquire(n)` | Try to consume n tokens. Returns true if successful. |
| `timeUntilAvailable(n)` | Duration until n tokens will be available. |
| `available()` | Current token count. |
| `reset()` | Reset to full capacity. |
| `capacity()` | Maximum tokens. |
| `refillRate()` | Tokens per second. |

## Examples

### Exchange Connector

```cpp
class BybitOrderExecutor : public IOrderExecutor {
public:
  BybitOrderExecutor()
    : _orderLimiter({.capacity = 10, .refillRate = 10})  // 10 orders/sec
  {}

  void submit(const Order& order) override {
    if (!_orderLimiter.tryAcquire()) {
      reject(order, "rate limit exceeded");
      return;
    }
    sendToExchange(order);
  }

private:
  RateLimiter _orderLimiter;
};
```

### Wait for Availability

```cpp
if (!limiter.tryAcquire()) {
  auto wait = limiter.timeUntilAvailable();
  std::this_thread::sleep_for(wait);
  limiter.tryAcquire();  // Should succeed now
}
```

### Multiple Token Operations

```cpp
// Batch request costs 5 tokens
if (limiter.tryAcquire(5)) {
  sendBatchRequest();
}
```

### Log Throttling

```cpp
class ThrottledLogger {
public:
  ThrottledLogger() : _limiter({.capacity = 10, .refillRate = 1}) {}  // 1/sec burst 10

  void warn(const std::string& msg) {
    if (_limiter.tryAcquire()) {
      std::cerr << "[WARN] " << msg << "\n";
    }
  }

private:
  RateLimiter _limiter;
};
```

## Algorithm

Token bucket with continuous refill:

1. Tokens accumulate at `refillRate` per second
2. Bucket holds maximum `capacity` tokens
3. `tryAcquire(n)` consumes n tokens if available
4. Lock-free implementation using atomics

## Thread Safety

All methods are thread-safe and lock-free. Safe for concurrent use from multiple threads.

## See Also

- [IKillSwitch](../killswitch/abstract_killswitch.md) — emergency shutdown
