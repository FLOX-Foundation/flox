/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "flox/util/concurrency/spsc_queue.h"

namespace
{

using namespace flox;

struct Tracked
{
  static std::atomic<int> live;
  int64_t value{0};

  Tracked() { ++live; }
  explicit Tracked(int64_t v) : value(v) { ++live; }
  Tracked(const Tracked& o) : value(o.value) { ++live; }
  Tracked(Tracked&& o) noexcept : value(o.value) { ++live; }
  Tracked& operator=(const Tracked&) = default;
  Tracked& operator=(Tracked&&) noexcept = default;
  ~Tracked() { --live; }
};

std::atomic<int> Tracked::live{0};

TEST(SPSCQueueSegments, EmptyQueueYieldsNoSegment)
{
  SPSCQueue<int, 8> q;
  int* seg = nullptr;
  EXPECT_EQ(q.read_segment(seg), 0u);
}

TEST(SPSCQueueSegments, WriteThenReadRoundTripsInOrder)
{
  SPSCQueue<int, 8> q;
  int* wseg = nullptr;
  const size_t writable = q.write_segment(wseg);
  ASSERT_GE(writable, 3u);
  for (int i = 0; i < 3; ++i)
  {
    new (&wseg[i]) int(i + 10);
  }
  q.commit_write(3);

  int* rseg = nullptr;
  const size_t readable = q.read_segment(rseg);
  ASSERT_EQ(readable, 3u);
  for (int i = 0; i < 3; ++i)
  {
    EXPECT_EQ(rseg[i], i + 10);
  }
  q.commit_read(3);
  EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueSegments, PartialCommitKeepsRemainderReadable)
{
  SPSCQueue<int, 8> q;
  int* wseg = nullptr;
  ASSERT_GE(q.write_segment(wseg), 5u);
  for (int i = 0; i < 5; ++i)
  {
    new (&wseg[i]) int(i);
  }
  q.commit_write(5);

  int* rseg = nullptr;
  ASSERT_EQ(q.read_segment(rseg), 5u);
  q.commit_read(2);  // consume a prefix only

  ASSERT_EQ(q.read_segment(rseg), 3u);
  EXPECT_EQ(rseg[0], 2);
  EXPECT_EQ(rseg[1], 3);
  EXPECT_EQ(rseg[2], 4);
  q.commit_read(3);
  EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueSegments, FullQueueYieldsNoWritableSegment)
{
  SPSCQueue<int, 4> q;
  int* wseg = nullptr;
  size_t total = 0;
  while (size_t n = q.write_segment(wseg))
  {
    for (size_t i = 0; i < n; ++i)
    {
      new (&wseg[i]) int(static_cast<int>(total + i));
    }
    q.commit_write(n);
    total += n;
  }
  EXPECT_EQ(total, 3u);  // capacity - 1 usable slots
  EXPECT_TRUE(q.full());
  EXPECT_EQ(q.write_segment(wseg), 0u);
}

TEST(SPSCQueueSegments, WrappedQueueDeliversEverythingInOrder)
{
  SPSCQueue<int64_t, 8> q;
  constexpr int64_t kTotal = 10'000;
  const size_t wchunk[] = {1, 3, 5, 2, 7, 4, 6};
  const size_t rchunk[] = {2, 1, 6, 3, 5, 7, 4};
  size_t wi = 0;
  size_t ri = 0;
  int64_t produced = 0;
  int64_t expect = 0;
  while (expect < kTotal)
  {
    if (produced < kTotal)
    {
      int64_t* seg = nullptr;
      size_t n = q.write_segment(seg);
      n = std::min({n, wchunk[wi++ % 7], static_cast<size_t>(kTotal - produced)});
      for (size_t k = 0; k < n; ++k)
      {
        new (&seg[k]) int64_t(produced + static_cast<int64_t>(k));
      }
      if (n != 0)
      {
        q.commit_write(n);
        produced += static_cast<int64_t>(n);
      }
    }
    int64_t* seg = nullptr;
    size_t n = q.read_segment(seg);
    n = std::min(n, rchunk[ri++ % 7]);
    for (size_t k = 0; k < n; ++k)
    {
      ASSERT_EQ(seg[k], expect);
      ++expect;
    }
    if (n != 0)
    {
      q.commit_read(n);
    }
  }
  EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueSegments, InteroperatesWithPerItemApi)
{
  SPSCQueue<int, 16> q;
  ASSERT_TRUE(q.try_emplace(1));
  ASSERT_TRUE(q.try_emplace(2));

  int* wseg = nullptr;
  ASSERT_GE(q.write_segment(wseg), 2u);
  new (&wseg[0]) int(3);
  new (&wseg[1]) int(4);
  q.commit_write(2);

  int* popped = q.try_pop();
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(*popped, 1);
  std::destroy_at(popped);

  int* rseg = nullptr;
  ASSERT_EQ(q.read_segment(rseg), 3u);
  EXPECT_EQ(rseg[0], 2);
  EXPECT_EQ(rseg[1], 3);
  EXPECT_EQ(rseg[2], 4);
  q.commit_read(3);
  EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueSegments, DestructorAccountingBalances)
{
  ASSERT_EQ(Tracked::live.load(), 0);
  {
    SPSCQueue<Tracked, 8> q;
    Tracked* wseg = nullptr;
    ASSERT_GE(q.write_segment(wseg), 4u);
    for (int i = 0; i < 4; ++i)
    {
      new (&wseg[i]) Tracked(i);
    }
    q.commit_write(4);
    EXPECT_EQ(Tracked::live.load(), 4);

    Tracked* rseg = nullptr;
    ASSERT_EQ(q.read_segment(rseg), 4u);
    for (int i = 0; i < 2; ++i)
    {
      EXPECT_EQ(rseg[i].value, i);
      rseg[i].~Tracked();
    }
    q.commit_read(2);
    EXPECT_EQ(Tracked::live.load(), 2);
    // remaining two are drained by the queue destructor
  }
  EXPECT_EQ(Tracked::live.load(), 0);
}

TEST(SPSCQueueSegments, TwoThreadStreamPreservesOrderAndCount)
{
  SPSCQueue<int64_t, 1024> q;
  constexpr int64_t kTotal = 1'000'000;

  std::thread producer(
      [&]
      {
        int64_t produced = 0;
        while (produced < kTotal)
        {
          int64_t* seg = nullptr;
          size_t n = q.write_segment(seg);
          if (n == 0)
          {
            std::this_thread::yield();
            continue;
          }
          n = std::min(n, static_cast<size_t>(kTotal - produced));
          for (size_t k = 0; k < n; ++k)
          {
            new (&seg[k]) int64_t(produced + static_cast<int64_t>(k));
          }
          q.commit_write(n);
          produced += static_cast<int64_t>(n);
        }
      });

  int64_t expect = 0;
  int64_t sum = 0;
  while (expect < kTotal)
  {
    int64_t* seg = nullptr;
    const size_t n = q.read_segment(seg);
    if (n == 0)
    {
      std::this_thread::yield();
      continue;
    }
    for (size_t k = 0; k < n; ++k)
    {
      ASSERT_EQ(seg[k], expect);
      sum += seg[k];
      ++expect;
    }
    q.commit_read(n);
  }
  producer.join();
  EXPECT_EQ(expect, kTotal);
  EXPECT_EQ(sum, kTotal * (kTotal - 1) / 2);
  EXPECT_TRUE(q.empty());
}

}  // namespace
