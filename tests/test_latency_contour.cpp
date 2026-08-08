/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "flox/util/performance/latency_contour.h"
#include "flox/util/performance/latency_histogram.h"

using flox::performance::LatencyContour;
using flox::performance::LatencyHistogram;
using flox::performance::monotonicNs;

TEST(LatencyHistogram, SmallValuesAreExact)
{
  LatencyHistogram h;
  for (int i = 0; i < 100; ++i)
  {
    h.record(7);
  }
  EXPECT_EQ(h.count(), 100u);
  EXPECT_EQ(h.max(), 7);
  EXPECT_EQ(h.quantile(0.5), 7);
  EXPECT_EQ(h.quantile(0.999), 7);
}

TEST(LatencyHistogram, QuantilesOnUniformRange)
{
  LatencyHistogram h;
  // 1..100000 ns uniformly: p50 ~ 50000 with ~3% bucket error.
  for (int64_t v = 1; v <= 100'000; ++v)
  {
    h.record(v);
  }
  const auto p50 = h.quantile(0.50);
  EXPECT_GT(p50, 46'000);
  EXPECT_LT(p50, 54'000);
  const auto p99 = h.quantile(0.99);
  EXPECT_GT(p99, 95'000);
  EXPECT_LE(p99, 106'000);
  EXPECT_EQ(h.max(), 100'000);
}

TEST(LatencyHistogram, TailValueDominatesMaxAndHighQuantile)
{
  LatencyHistogram h;
  for (int i = 0; i < 9999; ++i)
  {
    h.record(1'000);  // 1 us
  }
  h.record(50'000'000);  // one 50 ms outlier
  EXPECT_EQ(h.max(), 50'000'000);
  EXPECT_LT(h.quantile(0.5), 1'100);
  // p99.99 must land in the outlier's bucket range.
  EXPECT_GT(h.quantile(0.9999), 40'000'000);
}

TEST(LatencyHistogram, BucketMappingIsMonotonic)
{
  int prev = -1;
  const std::vector<int64_t> probes{0, 1, 31, 32, 33, 100, 1000, 4096, 1'000'000,
                                    1'000'000'000, int64_t(1) << 39};
  for (int64_t v : probes)
  {
    const int b = LatencyHistogram::bucketFor(v);
    EXPECT_GE(b, prev);
    EXPECT_GE(LatencyHistogram::bucketUpperBound(b), v);
    prev = b;
  }
}

TEST(LatencyHistogram, ConcurrentRecordingLosesNothing)
{
  LatencyHistogram h;
  constexpr int kThreads = 4;
  constexpr int kPerThread = 100'000;
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t)
  {
    ts.emplace_back([&h]
                    {
      for (int i = 0; i < kPerThread; ++i)
      {
        h.record(1000 + i % 64);
      } });
  }
  for (auto& t : ts)
  {
    t.join();
  }
  EXPECT_EQ(h.count(), uint64_t(kThreads) * kPerThread);
}

TEST(LatencyContour, SegmentsRecordIndependently)
{
  LatencyContour<8> contour;
  const auto parse = contour.registerSegment("parse");
  const auto bus = contour.registerSegment("bus");
  ASSERT_NE(parse, LatencyContour<8>::kInvalid);
  ASSERT_NE(bus, LatencyContour<8>::kInvalid);

  contour.record(parse, 500);
  contour.record(parse, 700);
  contour.recordSpan(bus, 1000, 3000);

  const auto snap = contour.snapshot();
  ASSERT_EQ(snap.size(), 2u);
  EXPECT_EQ(snap[0].name, "parse");
  EXPECT_EQ(snap[0].count, 2u);
  EXPECT_EQ(snap[1].name, "bus");
  EXPECT_EQ(snap[1].count, 1u);
  EXPECT_GE(snap[1].p50, 2000);

  const auto rep = contour.report();
  EXPECT_NE(rep.find("parse"), std::string::npos);
  EXPECT_NE(rep.find("bus"), std::string::npos);
}

TEST(LatencyContour, OverflowAndInvalidIdsAreSafe)
{
  LatencyContour<2> contour;
  EXPECT_NE(contour.registerSegment("a"), LatencyContour<2>::kInvalid);
  EXPECT_NE(contour.registerSegment("b"), LatencyContour<2>::kInvalid);
  EXPECT_EQ(contour.registerSegment("c"), LatencyContour<2>::kInvalid);
  contour.record(LatencyContour<2>::kInvalid, 100);  // must not crash
  contour.record(99, 100);                           // out of range: ignored
}

TEST(LatencyContour, MonotonicClockAdvances)
{
  const auto a = monotonicNs();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto b = monotonicNs();
  EXPECT_GT(b, a);
  EXPECT_LT(b - a, int64_t(1'000'000'000));  // sane bound: < 1s
}
