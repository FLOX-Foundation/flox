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
#include <thread>

using namespace flox;

namespace
{

struct Counter
{
  int value = 0;
  inline static int constructed = 0;
  inline static int destructed = 0;

  Counter() { ++constructed; }
  Counter(int v) : value(v) { ++constructed; }
  Counter(const Counter& other) : value(other.value) { ++constructed; }
  Counter(Counter&& other) noexcept : value(other.value) { ++constructed; }
  Counter& operator=(const Counter&) = default;
  Counter& operator=(Counter&&) = default;
  ~Counter() { ++destructed; }
};

struct SPSCQueueTest : public ::testing::Test
{
  void SetUp() override
  {
    Counter::constructed = 0;
    Counter::destructed = 0;
  }
};

constexpr size_t kCap = 8;
using Queue = SPSCQueue<Counter, kCap>;

// Basic push/copy and pop/copy roundtrip
TEST_F(SPSCQueueTest, PushPopRoundtrip)
{
  Queue q;
  Counter c{42};
  EXPECT_TRUE(q.push(c));
  Counter out;
  EXPECT_TRUE(q.pop(out));
  EXPECT_EQ(out.value, 42);
  EXPECT_TRUE(q.empty());
}

// Move-based emplace
TEST_F(SPSCQueueTest, EmplaceMoveSemantics)
{
  Queue q;
  Counter c{99};
  EXPECT_TRUE(q.emplace(std::move(c)));
  auto ref = q.try_pop_ref();
  ASSERT_TRUE(ref.has_value());
  EXPECT_EQ(ref->get().value, 99);
}

// try_emplace in-place construction
TEST_F(SPSCQueueTest, TryEmplaceWorks)
{
  Queue q;
  for (int i = 0; i < kCap - 1; ++i)
  {
    EXPECT_TRUE(q.try_emplace(i));
  }
  EXPECT_TRUE(q.full());
  EXPECT_FALSE(q.try_emplace(12345));
}

// try_pop pointer access and manual destruction
TEST_F(SPSCQueueTest, TryPopPointerAccess)
{
  Queue q;
  q.try_emplace(77);
  Counter* ptr = q.try_pop();
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(ptr->value, 77);
  ptr->~Counter();
}

// try_pop_ref returns non-copying access
TEST_F(SPSCQueueTest, TryPopRefWrapper)
{
  Queue q;
  q.try_emplace(888);
  auto ref = q.try_pop_ref();
  ASSERT_TRUE(ref.has_value());
  EXPECT_EQ(ref->get().value, 888);
}

// queue reports empty on construction
TEST_F(SPSCQueueTest, IsInitiallyEmpty)
{
  Queue q;
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
}

// queue reports full correctly
TEST_F(SPSCQueueTest, IsFullCorrectly)
{
  Queue q;
  for (int i = 0; i < kCap - 1; ++i)
  {
    q.try_emplace(i);
  }
  EXPECT_TRUE(q.full());
}

// wrap-around handling
TEST_F(SPSCQueueTest, WrapAroundCycles)
{
  Queue q;
  for (int round = 0; round < 3; ++round)
  {
    for (int i = 0; i < kCap - 1; ++i)
    {
      EXPECT_TRUE(q.try_emplace(i));
    }
    for (int i = 0; i < kCap - 1; ++i)
    {
      auto ref = q.try_pop_ref();
      ASSERT_TRUE(ref.has_value());
    }
  }
  EXPECT_TRUE(q.empty());
}

// destruction cleans up live elements
TEST_F(SPSCQueueTest, DestructorCleansUpInQueue)
{
  {
    Queue q;
    for (int i = 0; i < kCap - 1; ++i)
    {
      q.try_emplace(i);
    }
  }
  EXPECT_EQ(Counter::constructed, Counter::destructed);
}

// pop from empty returns false
TEST_F(SPSCQueueTest, PopFailsWhenEmpty)
{
  Queue q;
  Counter dummy;
  EXPECT_FALSE(q.pop(dummy));
}

// try_pop returns nullptr when empty
TEST_F(SPSCQueueTest, TryPopReturnsNullptrWhenEmpty)
{
  Queue q;
  EXPECT_EQ(q.try_pop(), nullptr);
}

// try_pop_ref returns nullopt when empty
TEST_F(SPSCQueueTest, TryPopRefReturnsNulloptWhenEmpty)
{
  Queue q;
  EXPECT_FALSE(q.try_pop_ref().has_value());
}

// size() tracks inserted elements
TEST_F(SPSCQueueTest, SizeTracksUsage)
{
  Queue q;
  for (int i = 0; i < 3; ++i)
  {
    q.try_emplace(i);
  }
  EXPECT_EQ(q.size(), 3u);
}

// multi-threaded producer-consumer
TEST_F(SPSCQueueTest, MultiThreadedPushPop)
{
  SPSCQueue<int, 1024> q;
  std::atomic<bool> running = true;
  std::atomic<int> produced = 0, consumed = 0;

  std::thread producer(
      [&]
      {
        for (int i = 0; i < 100000; ++i)
        {
          while (!q.push(i))
          {
          }
          ++produced;
        }
        running = false;
      });

  std::thread consumer(
      [&]
      {
        int val;
        while (running || !q.empty())
        {
          if (q.pop(val))
          {
            ++consumed;
          }
        }
      });

  producer.join();
  consumer.join();
  EXPECT_EQ(produced.load(), 100000);
  EXPECT_EQ(consumed.load(), 100000);
}

}  // namespace
