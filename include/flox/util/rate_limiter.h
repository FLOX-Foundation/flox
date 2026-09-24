/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace flox
{

/// Token bucket rate limiter.
/// Thread-safe and lock-free (a CAS loop, so lock-free but not wait-free: a
/// thread can retry under contention).
class RateLimiter
{
 public:
  using Clock = std::chrono::steady_clock;
  using Duration = std::chrono::nanoseconds;

  struct Config
  {
    uint32_t capacity;    ///< Maximum tokens in bucket
    uint32_t refillRate;  ///< Tokens added per second; 0 means "never refills"
  };

  /// A refillRate of 0 is a valid configuration, not an error: the bucket is a
  /// fixed burst budget of exactly `capacity` tokens that only reset() brings
  /// back. It used to be guarded by an assert alone, which NDEBUG removes from
  /// every shipped build and left refill() evaluating seconds(1) / 0.
  explicit RateLimiter(Config config)
      : _capacity(config.capacity), _refillRate(config.refillRate), _tokens(config.capacity), _lastRefill(Clock::now().time_since_epoch().count())
  {
  }

  /// Try to acquire tokens. Returns true if successful.
  [[nodiscard]] bool tryAcquire(uint32_t tokens = 1) noexcept
  {
    refill();

    uint32_t current = _tokens.load(std::memory_order_relaxed);
    while (current >= tokens)
    {
      if (_tokens.compare_exchange_weak(current, current - tokens, std::memory_order_acq_rel))
      {
        return true;
      }
    }
    return false;
  }

  /// Time until tokens become available. Duration::max() means "never": either
  /// the bucket does not refill at all, or the request is larger than the
  /// bucket can ever hold.
  [[nodiscard]] Duration timeUntilAvailable(uint32_t tokens = 1) const noexcept
  {
    uint32_t current = _tokens.load(std::memory_order_relaxed);
    if (current >= tokens)
    {
      return Duration::zero();
    }

    if (_refillRate == 0 || tokens > _capacity)
    {
      return Duration::max();
    }

    const uint32_t needed = tokens - current;
    return Duration(static_cast<int64_t>(needed) * nsPerToken());
  }

  /// Current available tokens
  uint32_t available() const noexcept
  {
    return _tokens.load(std::memory_order_relaxed);
  }

  /// Reset to full capacity
  void reset() noexcept
  {
    _tokens.store(_capacity, std::memory_order_relaxed);
    _lastRefill.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  }

  uint32_t capacity() const noexcept { return _capacity; }
  uint32_t refillRate() const noexcept { return _refillRate; }

 private:
  static constexpr int64_t kNanosPerSecond = 1'000'000'000;

  /// Nanoseconds per token, never zero. A rate above one token per nanosecond
  /// would round the period down to zero and make the division below a second
  /// division by zero; the bucket cannot be drained faster than that anyway,
  /// so clamping the period costs nothing.
  int64_t nsPerToken() const noexcept
  {
    const int64_t ns = kNanosPerSecond / static_cast<int64_t>(_refillRate);
    return ns > 0 ? ns : 1;
  }

  void refill() noexcept
  {
    if (_refillRate == 0)
    {
      return;
    }

    auto now = Clock::now();
    auto lastNs = _lastRefill.load(std::memory_order_relaxed);
    auto last = Clock::time_point(Duration(lastNs));

    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last);
    const int64_t perToken = nsPerToken();

    if (elapsed.count() < perToken)
    {
      return;
    }

    // Capped here rather than only after the add: an uncapped count overflows
    // the uint32_t cast after a long idle period and lands on an arbitrary
    // small number of tokens.
    int64_t tokensToAdd = elapsed.count() / perToken;
    if (tokensToAdd > static_cast<int64_t>(_capacity))
    {
      tokensToAdd = static_cast<int64_t>(_capacity);
    }
    if (tokensToAdd == 0)
    {
      return;
    }

    // Try to update last refill time
    auto newLastNs = (last + Duration(tokensToAdd * perToken)).time_since_epoch().count();
    if (!_lastRefill.compare_exchange_strong(lastNs, newLastNs, std::memory_order_acq_rel))
    {
      return;  // Another thread refilled
    }

    // Add tokens (capped at capacity)
    uint32_t current = _tokens.load(std::memory_order_relaxed);
    uint32_t newTokens;
    do
    {
      newTokens = static_cast<uint32_t>(
          std::min<uint64_t>(static_cast<uint64_t>(current) + static_cast<uint64_t>(tokensToAdd),
                             static_cast<uint64_t>(_capacity)));
    } while (!_tokens.compare_exchange_weak(current, newTokens, std::memory_order_acq_rel));
  }

  uint32_t _capacity;
  uint32_t _refillRate;
  std::atomic<uint32_t> _tokens;
  std::atomic<int64_t> _lastRefill;
};

}  // namespace flox
