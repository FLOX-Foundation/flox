/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Review finding 7 behind the execution tracker fix.
//
// RateLimiter guards refillRate == 0 with an assert only. CMakeLists.txt pins
// CMAKE_BUILD_TYPE to Release when it is unset, so NDEBUG is on in every
// shipped configuration and the guard is gone: refill() then evaluates
// nanoseconds(seconds(1)) / 0 and timeUntilAvailable() does the same. On x86
// that is SIGFPE; on aarch64 the division yields 0 and the limiter silently
// answers "a token is available now" forever. Both are wrong, and the
// division is undefined behaviour either way.
//
// A refillRate of 0 is a meaningful configuration -- a fixed burst budget that
// is never replenished until reset() -- so the limiter must handle it rather
// than die on it: grant exactly `capacity` tokens, then refuse, and report
// that the next token never arrives.

#include "flox/util/rate_limiter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace flox;

namespace
{

// "Never" for a bucket that is never refilled. Any finite answer here means
// the limiter is quietly refilling behind the caller's back (the clamp-to-1
// workaround) or divided by zero and produced garbage.
constexpr RateLimiter::Duration kNever =
    std::chrono::duration_cast<RateLimiter::Duration>(std::chrono::hours(24 * 365));

}  // namespace

TEST(RateLimiterZeroRefillTest, ZeroCapacityAndZeroRefillRefusesInsteadOfDividingByZero)
{
  RateLimiter limiter({.capacity = 0, .refillRate = 0});

  EXPECT_EQ(limiter.capacity(), 0u);
  EXPECT_EQ(limiter.refillRate(), 0u);
  EXPECT_EQ(limiter.available(), 0u);

  EXPECT_FALSE(limiter.tryAcquire());
  EXPECT_FALSE(limiter.tryAcquire(1));
  EXPECT_EQ(limiter.available(), 0u);
  EXPECT_GE(limiter.timeUntilAvailable(1), kNever);
}

TEST(RateLimiterZeroRefillTest, ZeroRefillGrantsExactlyTheBurstThenRefuses)
{
  constexpr uint32_t kBurst = 5;
  RateLimiter limiter({.capacity = kBurst, .refillRate = 0});

  for (uint32_t i = 0; i < kBurst; ++i)
  {
    EXPECT_TRUE(limiter.tryAcquire()) << "token " << i << " of " << kBurst;
    EXPECT_EQ(limiter.available(), kBurst - i - 1);
  }

  EXPECT_FALSE(limiter.tryAcquire());
  EXPECT_EQ(limiter.available(), 0u);
  EXPECT_GE(limiter.timeUntilAvailable(1), kNever);
}

TEST(RateLimiterZeroRefillTest, ZeroRefillNeverReplenishesOverTime)
{
  RateLimiter limiter({.capacity = 2, .refillRate = 0});

  EXPECT_TRUE(limiter.tryAcquire(2));
  EXPECT_EQ(limiter.available(), 0u);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_FALSE(limiter.tryAcquire());
  EXPECT_EQ(limiter.available(), 0u);
  EXPECT_GE(limiter.timeUntilAvailable(1), kNever);
}

TEST(RateLimiterZeroRefillTest, ZeroRefillBucketIsRefilledOnlyByReset)
{
  RateLimiter limiter({.capacity = 3, .refillRate = 0});

  EXPECT_TRUE(limiter.tryAcquire(3));
  EXPECT_FALSE(limiter.tryAcquire());

  limiter.reset();

  EXPECT_EQ(limiter.available(), 3u);
  EXPECT_EQ(limiter.timeUntilAvailable(1), RateLimiter::Duration::zero());
  EXPECT_TRUE(limiter.tryAcquire(3));
  EXPECT_FALSE(limiter.tryAcquire());
}

// The other end of the same division. A rate above one token per nanosecond
// rounds the period down to zero, which is a second division by zero in
// refill() and a "no wait at all" answer from timeUntilAvailable(). The period
// is clamped to 1 ns: the bucket cannot be drained faster than that anyway.
TEST(RateLimiterZeroRefillTest, RefillRateAboveOneTokenPerNanosecondHasADefinedPeriod)
{
  RateLimiter limiter({.capacity = 4, .refillRate = 4'000'000'000u});

  EXPECT_EQ(limiter.capacity(), 4u);
  EXPECT_EQ(limiter.refillRate(), 4'000'000'000u);

  EXPECT_TRUE(limiter.tryAcquire(4));
  EXPECT_EQ(limiter.available(), 0u);

  const auto wait = limiter.timeUntilAvailable(1);
  EXPECT_GT(wait, RateLimiter::Duration::zero());
  EXPECT_LE(wait, RateLimiter::Duration(1));

  // refill() divides by the same period, so it must not divide by zero here
  // either: one millisecond is far more than the whole bucket's worth of
  // tokens, and the refill caps at capacity.
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_TRUE(limiter.tryAcquire());
  EXPECT_EQ(limiter.available(), 3u);
}

// A non-zero refill rate must keep behaving exactly as before: the fix is a
// guard on the zero case, not a change of the token-bucket semantics.
TEST(RateLimiterZeroRefillTest, NonZeroRefillRateIsUnaffected)
{
  RateLimiter limiter({.capacity = 10, .refillRate = 1000});  // 1 token per ms

  EXPECT_TRUE(limiter.tryAcquire(10));
  EXPECT_EQ(limiter.available(), 0u);
  EXPECT_EQ(limiter.timeUntilAvailable(1), std::chrono::milliseconds(1));
  // Exactly the bucket's capacity is a finite wait; only a request the bucket
  // can never hold at once is "never".
  EXPECT_EQ(limiter.timeUntilAvailable(10), std::chrono::milliseconds(10));
  EXPECT_EQ(limiter.timeUntilAvailable(11), RateLimiter::Duration::max());

  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  EXPECT_TRUE(limiter.tryAcquire());
}
