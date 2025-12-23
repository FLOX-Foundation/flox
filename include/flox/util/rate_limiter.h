/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <atomic>
#include <chrono>

namespace flox
{

/// Token bucket rate limiter
/// Thread-safe, lock-free implementation
class RateLimiter
{
 public:
  using Clock = std::chrono::steady_clock;
  using Duration = std::chrono::nanoseconds;

  struct Config
  {
    uint32_t capacity;    ///< Maximum tokens in bucket
    uint32_t refillRate;  ///< Tokens added per second
  };

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

  /// Time until tokens become available
  [[nodiscard]] Duration timeUntilAvailable(uint32_t tokens = 1) const noexcept
  {
    uint32_t current = _tokens.load(std::memory_order_relaxed);
    if (current >= tokens)
    {
      return Duration::zero();
    }

    uint32_t needed = tokens - current;
    auto nsPerToken = std::chrono::nanoseconds(std::chrono::seconds(1)) / _refillRate;
    return Duration(needed * nsPerToken.count());
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
  void refill() noexcept
  {
    auto now = Clock::now();
    auto lastNs = _lastRefill.load(std::memory_order_relaxed);
    auto last = Clock::time_point(Duration(lastNs));

    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last);
    auto nsPerToken = std::chrono::nanoseconds(std::chrono::seconds(1)) / _refillRate;

    if (elapsed < nsPerToken)
    {
      return;
    }

    auto tokensToAdd = static_cast<uint32_t>(elapsed.count() / nsPerToken.count());
    if (tokensToAdd == 0)
    {
      return;
    }

    // Try to update last refill time
    auto newLastNs = (last + tokensToAdd * nsPerToken).time_since_epoch().count();
    if (!_lastRefill.compare_exchange_strong(lastNs, newLastNs, std::memory_order_acq_rel))
    {
      return;  // Another thread refilled
    }

    // Add tokens (capped at capacity)
    uint32_t current = _tokens.load(std::memory_order_relaxed);
    uint32_t newTokens;
    do
    {
      newTokens = std::min(current + tokensToAdd, _capacity);
    } while (!_tokens.compare_exchange_weak(current, newTokens, std::memory_order_acq_rel));
  }

  uint32_t _capacity;
  uint32_t _refillRate;
  std::atomic<uint32_t> _tokens;
  std::atomic<int64_t> _lastRefill;
};

}  // namespace flox
