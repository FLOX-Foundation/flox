/*
 * FLOX Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/util/concurrency/thread_body.h"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

using namespace flox;

namespace
{

// The whole point of the helper is that the process is still here to run the
// next line. If the containment is removed, an escaping exception terminates
// the test binary and every one of these turns red at once -- which is the
// intended signal, not an accident of the harness.
TEST(ThreadBody, ProcessSurvivesAnExceptionThrownInAThreadBody)
{
  const uint64_t before = threadDeathCount();

  std::thread t([]
                { runThreadBody("test.thrower", []
                                { throw std::runtime_error("boom"); }); });
  t.join();

  EXPECT_EQ(threadDeathCount(), before + 1);
}

// Containment that says nothing turns a loud death into a quiet one. The
// count and the name are the only evidence a supervisor gets, so both are
// part of the contract.
TEST(ThreadBody, TheDeadThreadNamesItself)
{
  std::thread t([]
                { runThreadBody("test.named", []
                                { throw std::runtime_error("boom"); }); });
  t.join();

  ASSERT_NE(lastDeadThreadName(), nullptr);
  EXPECT_STREQ(lastDeadThreadName(), "test.named");
}

// Not every throw is a std::exception. A thrown int from a C-ish library must
// be contained by the same net, or the net has a hole exactly where the least
// well-behaved code is.
TEST(ThreadBody, ContainsWhatIsNotAStdException)
{
  const uint64_t before = threadDeathCount();

  std::thread t([]
                { runThreadBody("test.foreign", []
                                { throw 42; }); });
  t.join();

  EXPECT_EQ(threadDeathCount(), before + 1);
  EXPECT_STREQ(lastDeadThreadName(), "test.foreign");
}

// A body that returns normally must leave no trace: a counter that also moved
// on success would be useless for telling the two apart.
TEST(ThreadBody, ABodyThatReturnsNormallyIsNotCountedAsDead)
{
  const uint64_t before = threadDeathCount();
  std::atomic<bool> ran{false};

  std::thread t([&ran]
                { runThreadBody("test.quiet", [&ran]
                                { ran.store(true); }); });
  t.join();

  EXPECT_TRUE(ran.load());
  EXPECT_EQ(threadDeathCount(), before);
}

// Every death is counted, including when they arrive from several threads at
// once. This does NOT prove the increment is atomic: the report writes to
// stderr, and the stream lock serializes the whole path, so a load-add-store
// would pass here too. The increment stays atomic because it is correct and
// costs nothing, not because this test defends it.
TEST(ThreadBody, CountsEveryDeathWhenSeveralThreadsDie)
{
  const uint64_t before = threadDeathCount();
  constexpr int kThreads = 16;

  std::vector<std::thread> ts;
  ts.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i)
  {
    ts.emplace_back([]
                    { runThreadBody("test.crowd", []
                                    { throw std::runtime_error("boom"); }); });
  }
  for (auto& t : ts)
  {
    t.join();
  }

  EXPECT_EQ(threadDeathCount(), before + kThreads);
}

}  // namespace
