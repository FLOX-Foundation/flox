/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string_view>

#include "flox/common.h"
#include "flox/log/log.h"
#include "flox/util/base/time.h"

namespace demo
{

class LatencyCollector
{
 public:
  enum LabelId : size_t
  {
    BusPublish,
    StrategyOnTrade,
    ExecutionOnFill,
    EndToEnd,
    LabelCount
  };

  static constexpr std::array<std::string_view, LabelCount> Labels{
      "bus_publish", "strategy_onTrade", "execution_onOrderFilled", "end_to_end"};

  static constexpr size_t MaxSamples = 1 << 20;  // 1M

  void record(LabelId id, std::chrono::nanoseconds delta)
  {
    if (_count[id] < MaxSamples)
    {
      _samples[id][_count[id]++] = delta.count();
    }
  }

  void report() const
  {
    for (size_t i = 0; i < LabelCount; ++i)
    {
      const auto n = _count[i];
      if (n == 0 || n > MaxSamples)
      {
        continue;
      }

      // Allocate buffer on heap
      std::unique_ptr<int64_t[]> sorted = std::make_unique<int64_t[]>(n);
      std::memcpy(sorted.get(), _samples[i].data(), n * sizeof(int64_t));

      std::sort(sorted.get(), sorted.get() + n);

      double mean = std::accumulate(sorted.get(), sorted.get() + n, 0.0) / n;
      int64_t p50 = sorted[n / 2];
      int64_t p95 = sorted[(n * 95) / 100];
      int64_t max = sorted[n - 1];

      FLOX_LOG(
          "[latency] "
          << Labels[i]
          << " | count=" << n
          << " mean=" << mean << "ns"
          << " p50=" << p50 << "ns"
          << " p95=" << p95 << "ns"
          << " max=" << max << "ns");
    }
  }

 private:
  inline static std::array<std::array<int64_t, MaxSamples>, LabelCount> _samples{};
  inline static std::array<size_t, LabelCount> _count{};
};

}  // namespace demo

#define MEASURE_LATENCY(label_id)                                     \
  auto __latency_start_##__LINE__ = std::chrono::steady_clock::now(); \
  demo::LatencyGuard __latency_guard_##__LINE__(label_id, __latency_start_##__LINE__);

extern demo::LatencyCollector collector;

namespace demo
{

class LatencyGuard
{
 public:
  LatencyGuard(LatencyCollector::LabelId id, flox::TimePoint start)
      : _id(id), _start(start) {}

  ~LatencyGuard()
  {
    auto end = std::chrono::steady_clock::now();
    collector.record(_id, end - _start);
  }

 private:
  LatencyCollector::LabelId _id;
  flox::TimePoint _start;
};

}  // namespace demo
