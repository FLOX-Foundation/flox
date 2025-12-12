/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/util/concurrency/spsc_queue.h"

#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <thread>

#ifndef _WIN32
#include <csignal>
#endif

using namespace flox;

namespace
{

struct FileWrapper
{
  FILE* f = nullptr;
  static inline int destructed = 0;

  FileWrapper(const char* path) { f = fopen(path, "w"); }
  ~FileWrapper()
  {
    if (f)
    {
      fclose(f);
    }
    ++destructed;
  }
};

TEST(SPSCQueueAdvancedTest, RAIIObjectsDestroyedProperly)
{
  FileWrapper::destructed = 0;
  {
    SPSCQueue<FileWrapper, 4> q;
    q.try_emplace("/tmp/test_raii_1.txt");
    q.try_emplace("/tmp/test_raii_2.txt");
    // Queue destructs → RAII should trigger fclose
  }
  EXPECT_EQ(FileWrapper::destructed, 2);
}

// Simulated UB: double destruction (intentionally wrong)
// Helper struct for death test - must be at namespace scope for MSVC
struct DeathTestDummy
{
  int value = 42;
  bool* destroyed = nullptr;

  ~DeathTestDummy()
  {
    if (destroyed && *destroyed)
    {
      std::abort();
    }
    if (destroyed)
    {
      *destroyed = true;
    }
  }
};

// Helper function for death test - MSVC doesn't support GCC statement expressions ({...})
void triggerDoubleDestruction()
{
  bool destroyed = false;
  SPSCQueue<DeathTestDummy, 4> q;
  DeathTestDummy d{42, &destroyed};
  q.push(d);  // creates internal copy
  DeathTestDummy out;
  q.pop(out);  // destroys queue copy
  // now `out` will destroy again on scope exit → abort
}

#ifdef _WIN32
TEST(SPSCQueueAdvancedTest, DoubleDestructionCausesAbort)
{
  ASSERT_EXIT(triggerDoubleDestruction(), ::testing::ExitedWithCode(3), ".*");
}
#else
TEST(SPSCQueueAdvancedTest, DoubleDestructionCausesAbort)
{
  ASSERT_EXIT(triggerDoubleDestruction(), ::testing::KilledBySignal(SIGABRT), ".*");
}
#endif

// Stress test
TEST(SPSCQueueAdvancedTest, StressTestMillionsOfOps)
{
  SPSCQueue<int, 1024> q;
  std::atomic<bool> done = false;
  std::atomic<int> count = 0;

  std::thread producer(
      [&]
      {
        for (int i = 0; i < 1'000'000; ++i)
        {
          while (!q.push(i))
          {
          }
        }
        done = true;
      });

  std::thread consumer(
      [&]
      {
        int val;
        while (!done || !q.empty())
        {
          if (q.pop(val))
          {
            ++count;
          }
        }
      });

  producer.join();
  consumer.join();

  EXPECT_EQ(count, 1'000'000);
}

}  // namespace
