/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "flox/util/performance/latency_histogram.h"

namespace flox::performance
{

// Monotonic nanoseconds from steady_clock. On Linux and macOS this reads the
// monotonic clock (the vDSO fast path on Linux); portable across platforms.
inline int64_t monotonicNs() noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Per-hop latency contour: a fixed set of named segments (parse, bus,
// strategy, risk, send, tick-to-order, ...), one histogram per segment,
// recording cheap enough to stay on in production. Answers "where did the
// microsecond go" with a distribution per hop instead of a debate.
//
// Segment registration happens at wiring time (not thread-safe); recording
// is wait-free and thread-safe.
template <size_t MaxSegments = 32>
class LatencyContour
{
 public:
  using SegmentId = uint32_t;
  static constexpr SegmentId kInvalid = UINT32_MAX;

  SegmentId registerSegment(std::string_view name)
  {
    const auto n = _count.load(std::memory_order_relaxed);
    if (n >= MaxSegments)
    {
      return kInvalid;
    }
    _names[n] = std::string(name);
    _count.store(n + 1, std::memory_order_release);
    return n;
  }

  void record(SegmentId id, int64_t ns) noexcept
  {
    if (id < _count.load(std::memory_order_acquire))
    {
      _hist[id].record(ns);
    }
  }

  void recordSpan(SegmentId id, int64_t startNs, int64_t endNs) noexcept
  {
    record(id, endNs - startNs);
  }

  struct SegmentSnapshot
  {
    std::string name;
    uint64_t count{0};
    int64_t p50{0};
    int64_t p99{0};
    int64_t p999{0};
    int64_t max{0};
  };

  std::vector<SegmentSnapshot> snapshot() const
  {
    std::vector<SegmentSnapshot> out;
    const auto n = _count.load(std::memory_order_acquire);
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      out.push_back(SegmentSnapshot{_names[i], _hist[i].count(), _hist[i].quantile(0.5),
                                    _hist[i].quantile(0.99), _hist[i].quantile(0.999),
                                    _hist[i].max()});
    }
    return out;
  }

  std::string report() const
  {
    std::string out;
    const auto n = _count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
    {
      out += _names[i];
      out += ": ";
      out += _hist[i].summary();
      out += '\n';
    }
    return out;
  }

  void reset()
  {
    const auto n = _count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
    {
      _hist[i].reset();
    }
  }

  LatencyHistogram& histogram(SegmentId id) { return _hist[id]; }

 private:
  LatencyHistogram _hist[MaxSegments]{};
  std::string _names[MaxSegments]{};
  std::atomic<uint32_t> _count{0};
};

}  // namespace flox::performance
