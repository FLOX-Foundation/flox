/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <flox/common.h>
#include <flox/util/base/move_only_function.h>
#include <flox/util/rate_limiter.h>

#include <chrono>
#include <cstdint>

namespace flox
{

/// Policy for handling rate limit exceeded
enum class RateLimitPolicy
{
  REJECT,   ///< Immediately reject the order
  WAIT,     ///< Block and wait until tokens available
  CALLBACK  ///< Call user-provided callback with wait time
};

/// Rate limit configuration for order executors
/// No default values - must be explicitly configured
struct RateLimitConfig
{
  uint32_t capacity;    ///< Max burst tokens (required)
  uint32_t refillRate;  ///< Tokens per second (required)

  RateLimitPolicy policy;

  /// Callback for CALLBACK policy: receives OrderId and wait duration
  using RateLimitCallback = MoveOnlyFunction<void(OrderId, std::chrono::nanoseconds waitTime)>;
  RateLimitCallback onRateLimited;

  bool isValid() const { return capacity > 0 && refillRate > 0; }

  RateLimiter::Config toRateLimiterConfig() const
  {
    return RateLimiter::Config{capacity, refillRate};
  }
};

}  // namespace flox
