/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <benchmark/benchmark.h>

#include "flox/util/performance/latency_contour.h"
#include "flox/util/performance/latency_histogram.h"

using flox::performance::LatencyContour;
using flox::performance::LatencyHistogram;
using flox::performance::monotonicNs;

// The contour stays on in production; the budget for one record() is tens of
// nanoseconds. This benchmark is the enforcement of that promise.

static void BM_HistogramRecord(benchmark::State& state)
{
  LatencyHistogram h;
  int64_t v = 1;
  for (auto _ : state)
  {
    h.record(v);
    v = (v * 2862933555777941757LL + 3037000493LL) & 0xFFFFF;  // cheap lcg spread
  }
  benchmark::DoNotOptimize(h.count());
}
BENCHMARK(BM_HistogramRecord);

static void BM_ContourRecordSpan(benchmark::State& state)
{
  LatencyContour<8> contour;
  const auto seg = contour.registerSegment("bench");
  int64_t t = 0;
  for (auto _ : state)
  {
    contour.recordSpan(seg, t, t + 1234);
    ++t;
  }
  benchmark::DoNotOptimize(contour.snapshot().size());
}
BENCHMARK(BM_ContourRecordSpan);

static void BM_MonotonicNs(benchmark::State& state)
{
  int64_t acc = 0;
  for (auto _ : state)
  {
    acc += monotonicNs();
  }
  benchmark::DoNotOptimize(acc);
}
BENCHMARK(BM_MonotonicNs);

static void BM_FullMeasurement(benchmark::State& state)
{
  // The realistic unit: two clock reads plus one record.
  LatencyContour<8> contour;
  const auto seg = contour.registerSegment("full");
  for (auto _ : state)
  {
    const auto t0 = monotonicNs();
    benchmark::ClobberMemory();
    const auto t1 = monotonicNs();
    contour.recordSpan(seg, t0, t1);
  }
  benchmark::DoNotOptimize(contour.snapshot().size());
}
BENCHMARK(BM_FullMeasurement);

static void BM_HistogramQuantile(benchmark::State& state)
{
  LatencyHistogram h;
  for (int64_t v = 1; v <= 1'000'000; v += 7)
  {
    h.record(v & 0xFFFFF);
  }
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(h.quantile(0.999));
  }
}
BENCHMARK(BM_HistogramQuantile);

BENCHMARK_MAIN();
