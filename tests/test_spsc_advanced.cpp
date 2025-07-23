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
TEST(SPSCQueueAdvancedTest, DoubleDestructionCausesAbort)
{
  ASSERT_EXIT(({
                struct Dummy
                {
                  int value = 42;
                  bool* destroyed = nullptr;

                  ~Dummy()
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

                bool destroyed = false;
                SPSCQueue<Dummy, 4> q;
                Dummy d{42, &destroyed};
                q.push(d);  // creates internal copy
                Dummy out;
                q.pop(out);  // destroys queue copy
                // now `out` will destroy again on scope exit
              }),
              ::testing::KilledBySignal(SIGABRT), ".*");
}

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
