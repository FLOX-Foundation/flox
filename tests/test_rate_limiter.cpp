/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/util/rate_limiter.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace flox;

TEST(RateLimiterTest, BasicAcquire)
{
  RateLimiter limiter({.capacity = 10, .refillRate = 10});

  EXPECT_EQ(limiter.available(), 10u);
  EXPECT_TRUE(limiter.tryAcquire());
  EXPECT_EQ(limiter.available(), 9u);
}

TEST(RateLimiterTest, AcquireMultiple)
{
  RateLimiter limiter({.capacity = 10, .refillRate = 10});

  EXPECT_TRUE(limiter.tryAcquire(5));
  EXPECT_EQ(limiter.available(), 5u);

  EXPECT_TRUE(limiter.tryAcquire(5));
  EXPECT_EQ(limiter.available(), 0u);
}

TEST(RateLimiterTest, ExhaustedBucket)
{
  RateLimiter limiter({.capacity = 3, .refillRate = 10});

  EXPECT_TRUE(limiter.tryAcquire());
  EXPECT_TRUE(limiter.tryAcquire());
  EXPECT_TRUE(limiter.tryAcquire());
  EXPECT_FALSE(limiter.tryAcquire());
  EXPECT_EQ(limiter.available(), 0u);
}

TEST(RateLimiterTest, CannotAcquireMoreThanAvailable)
{
  RateLimiter limiter({.capacity = 5, .refillRate = 10});

  EXPECT_FALSE(limiter.tryAcquire(6));
  EXPECT_EQ(limiter.available(), 5u);  // Unchanged
}

TEST(RateLimiterTest, Refill)
{
  RateLimiter limiter({.capacity = 10, .refillRate = 100});  // 100/sec = 1 per 10ms

  // Exhaust bucket
  EXPECT_TRUE(limiter.tryAcquire(10));
  EXPECT_EQ(limiter.available(), 0u);

  // Wait for refill
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Should have some tokens now
  EXPECT_TRUE(limiter.tryAcquire());
}

TEST(RateLimiterTest, RefillCappedAtCapacity)
{
  RateLimiter limiter({.capacity = 5, .refillRate = 100});

  // Use one token
  (void)limiter.tryAcquire();
  EXPECT_EQ(limiter.available(), 4u);

  // Wait long enough to refill more than capacity
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Trigger refill by trying to acquire
  (void)limiter.tryAcquire();

  // Should be capped at capacity - 1 (just acquired one)
  EXPECT_LE(limiter.available(), 4u);
}

TEST(RateLimiterTest, TimeUntilAvailable)
{
  RateLimiter limiter({.capacity = 10, .refillRate = 10});  // 10/sec = 100ms per token

  // Full bucket - no wait
  EXPECT_EQ(limiter.timeUntilAvailable().count(), 0);

  // Exhaust
  (void)limiter.tryAcquire(10);

  // Need to wait for 1 token
  auto wait = limiter.timeUntilAvailable();
  EXPECT_GT(wait.count(), 0);
  EXPECT_LE(wait, std::chrono::milliseconds(100));
}

TEST(RateLimiterTest, Reset)
{
  RateLimiter limiter({.capacity = 10, .refillRate = 10});

  (void)limiter.tryAcquire(10);
  EXPECT_EQ(limiter.available(), 0u);

  limiter.reset();
  EXPECT_EQ(limiter.available(), 10u);
}

TEST(RateLimiterTest, ConcurrentAccess)
{
  RateLimiter limiter({.capacity = 1000, .refillRate = 1});

  std::atomic<int> successCount{0};
  std::vector<std::thread> threads;

  for (int i = 0; i < 10; ++i)
  {
    threads.emplace_back(
        [&]()
        {
          for (int j = 0; j < 100; ++j)
          {
            if (limiter.tryAcquire())
            {
              ++successCount;
            }
          }
        });
  }

  for (auto& t : threads)
  {
    t.join();
  }

  // Exactly 1000 should succeed (capacity)
  EXPECT_EQ(successCount.load(), 1000);
  EXPECT_EQ(limiter.available(), 0u);
}

TEST(RateLimiterTest, HighRefillRate)
{
  RateLimiter limiter({.capacity = 100, .refillRate = 10000});  // 10k/sec

  (void)limiter.tryAcquire(100);
  EXPECT_EQ(limiter.available(), 0u);

  // Wait 50ms - should refill ~500 tokens, capped at 100
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Trigger refill
  EXPECT_TRUE(limiter.tryAcquire());
}
