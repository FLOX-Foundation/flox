/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/engine/tick_barrier.h"
#include "flox/engine/tick_guard.h"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

using namespace flox;

TEST(TickBarrierTest, WaitBlocksUntilAllComplete)
{
  TickBarrier barrier(3);
  std::atomic<bool> done{false};

  std::thread waiter(
      [&]
      {
        barrier.wait();
        done.store(true, std::memory_order_release);
      });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(done.load(std::memory_order_acquire));

  barrier.complete();  // 1
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(done.load(std::memory_order_acquire));

  barrier.complete();  // 2
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(done.load(std::memory_order_acquire));

  barrier.complete();  // 3
  waiter.join();

  EXPECT_TRUE(done.load(std::memory_order_acquire));
}

TEST(TickGuardTest, DestructorCallsComplete)
{
  TickBarrier barrier(1);

  {
    TickGuard guard(barrier);
  }

  barrier.wait();  // should not block
  SUCCEED();
}

TEST(TickBarrierTest, StressTestWithManyThreads)
{
  constexpr size_t threadCount = 128;
  TickBarrier barrier(threadCount);
  std::atomic<size_t> counter{0};

  std::vector<std::thread> threads;
  threads.reserve(threadCount);

  for (size_t i = 0; i < threadCount; ++i)
  {
    threads.emplace_back(
        [&]
        {
          // Simulate work
          std::this_thread::sleep_for(std::chrono::milliseconds(1 + (rand() % 5)));
          counter.fetch_add(1, std::memory_order_relaxed);
          barrier.complete();
        });
  }

  barrier.wait();
  for (auto& t : threads)
  {
    t.join();
  }

  EXPECT_EQ(counter.load(), threadCount);
}
