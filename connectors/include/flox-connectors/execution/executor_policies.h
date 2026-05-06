/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox-connectors/execution/timeout_order_tracker.h"
#include "flox-connectors/util/rate_limit_config.h"

#include <flox/common.h>
#include <flox/log/log.h>
#include <flox/util/rate_limiter.h>

#include <chrono>
#include <thread>

namespace flox
{

// ============================================================================
// Rate Limit Policies - compile-time dispatch, zero overhead when disabled
// ============================================================================

/// No rate limiting - zero overhead
struct NoRateLimitPolicy
{
  static constexpr bool enabled = false;

  void init(const RateLimitConfig&) {}
  [[nodiscard]] bool tryAcquire(OrderId) { return true; }
};

/// Active rate limiting with configurable behavior
class ActiveRateLimitPolicy
{
 public:
  static constexpr bool enabled = true;

  void init(RateLimitConfig config)
  {
    _config = std::move(config);
    if (_config.isValid())
    {
      _limiter.emplace(_config.toRateLimiterConfig());
    }
  }

  [[nodiscard]] bool tryAcquire(OrderId orderId)
  {
    if (!_limiter)
    {
      return true;
    }

    if (_limiter->tryAcquire())
    {
      return true;
    }

    auto waitTime = _limiter->timeUntilAvailable();

    switch (_config.policy)
    {
      case RateLimitPolicy::REJECT:
        FLOX_LOG_WARN("[RateLimit] Rejected orderId="
                      << orderId << " wait="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(waitTime).count()
                      << "ms");
        return false;

      case RateLimitPolicy::WAIT:
        std::this_thread::sleep_for(waitTime);
        (void)_limiter->tryAcquire();
        return true;

      case RateLimitPolicy::CALLBACK:
        if (_config.onRateLimited)
        {
          _config.onRateLimited(orderId, waitTime);
        }
        return false;
    }
    return false;
  }

 private:
  RateLimitConfig _config{};
  std::optional<RateLimiter> _limiter;
};

// ============================================================================
// Timeout Tracking Policies - compile-time dispatch, zero overhead when disabled
// ============================================================================

/// No timeout tracking - zero overhead
struct NoTimeoutPolicy
{
  static constexpr bool enabled = false;

  void init(const OrderTimeoutConfig&) {}
  void start() {}
  void trackSubmit(OrderId) {}
  void trackCancel(OrderId) {}
  void trackReplace(OrderId) {}
  void clearPending(OrderId) {}
};

/// Active timeout tracking
class ActiveTimeoutPolicy
{
 public:
  static constexpr bool enabled = true;

  void init(OrderTimeoutConfig config)
  {
    if (config.isValid())
    {
      _tracker = std::make_unique<TimeoutOrderTracker>(std::move(config));
    }
  }

  void start()
  {
    if (_tracker)
    {
      _tracker->start();
    }
  }

  void trackSubmit(OrderId id)
  {
    if (_tracker)
    {
      _tracker->trackSubmit(id);
    }
  }

  void trackCancel(OrderId id)
  {
    if (_tracker)
    {
      _tracker->trackCancel(id);
    }
  }

  void trackReplace(OrderId id)
  {
    if (_tracker)
    {
      _tracker->trackReplace(id);
    }
  }

  void clearPending(OrderId id)
  {
    if (_tracker)
    {
      _tracker->clearPending(id);
    }
  }

 private:
  std::unique_ptr<TimeoutOrderTracker> _tracker;
};

// ============================================================================
// Policy Bundle - combines rate limit and timeout policies
// ============================================================================

template <typename RateLimitPolicyT = NoRateLimitPolicy, typename TimeoutPolicyT = NoTimeoutPolicy>
struct ExecutorPolicies
{
  using RateLimitType = RateLimitPolicyT;
  using TimeoutType = TimeoutPolicyT;

  RateLimitPolicyT rateLimit;
  TimeoutPolicyT timeout;
};

// Common policy configurations
using NoPolicies = ExecutorPolicies<NoRateLimitPolicy, NoTimeoutPolicy>;
using WithRateLimit = ExecutorPolicies<ActiveRateLimitPolicy, NoTimeoutPolicy>;
using WithTimeout = ExecutorPolicies<NoRateLimitPolicy, ActiveTimeoutPolicy>;
using FullPolicies = ExecutorPolicies<ActiveRateLimitPolicy, ActiveTimeoutPolicy>;

}  // namespace flox
