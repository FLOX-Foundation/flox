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
#include <bit>
#include <cstdint>
#include <sstream>
#include <string>

namespace flox::performance
{

// Log-bucketed latency histogram in the HdrHistogram spirit, sized for
// nanosecond spans from 1ns to ~18 minutes. Tail-first by construction:
// storage cost is fixed, precision is relative (~3% per bucket), recording is
// a handful of bit operations plus one relaxed increment -- safe to keep on
// in production, which is the whole point: latency is measured continuously,
// not in a lab.
//
// Recording is wait-free and thread-safe (relaxed atomics). Snapshots taken
// while writers are active are approximate -- fine for monitoring; quiesce
// writers for exact numbers (tests, benchmarks).
class LatencyHistogram
{
 public:
  static constexpr int kSubBits = 5;                // 32 sub-buckets per octave
  static constexpr int kSub = 1 << kSubBits;        // 32
  static constexpr int kOctaves = 41;               // up to 2^40 ns (~18.3 min)
  static constexpr int kBuckets = kOctaves * kSub;  // 1312

  void record(int64_t ns) noexcept
  {
    _counts[bucketFor(ns)].fetch_add(1, std::memory_order_relaxed);
    _total.fetch_add(1, std::memory_order_relaxed);

    // Exact max: CAS loop is fine, contended only when a new maximum lands.
    int64_t seen = _max.load(std::memory_order_relaxed);
    while (ns > seen && !_max.compare_exchange_weak(seen, ns, std::memory_order_relaxed))
    {
    }
  }

  uint64_t count() const noexcept { return _total.load(std::memory_order_relaxed); }
  int64_t max() const noexcept { return _max.load(std::memory_order_relaxed); }

  // q in [0, 1]; returns the upper bound of the bucket holding that quantile.
  int64_t quantile(double q) const noexcept
  {
    const uint64_t total = count();
    if (total == 0)
    {
      return 0;
    }
    const auto target = static_cast<uint64_t>(q * static_cast<double>(total));
    uint64_t seen = 0;
    for (int b = 0; b < kBuckets; ++b)
    {
      seen += _counts[b].load(std::memory_order_relaxed);
      if (seen > target)
      {
        return bucketUpperBound(b);
      }
    }
    return max();
  }

  void reset() noexcept
  {
    for (auto& c : _counts)
    {
      c.store(0, std::memory_order_relaxed);
    }
    _total.store(0, std::memory_order_relaxed);
    _max.store(0, std::memory_order_relaxed);
  }

  std::string summary() const
  {
    std::ostringstream os;
    os << "n=" << count()
       << " p50=" << quantile(0.50)
       << " p90=" << quantile(0.90)
       << " p99=" << quantile(0.99)
       << " p99.9=" << quantile(0.999)
       << " max=" << max() << " (ns)";
    return os.str();
  }

  static int bucketFor(int64_t ns) noexcept
  {
    if (ns < kSub)
    {
      return ns < 0 ? 0 : static_cast<int>(ns);  // exact buckets below 32ns
    }
    const auto v = static_cast<uint64_t>(ns);
    const int octave = 63 - std::countl_zero(v);  // floor(log2 v), >= 5
    const int shifted = static_cast<int>(v >> (octave - kSubBits)) & (kSub - 1);
    const int idx = octave * kSub + shifted;
    return idx < kBuckets ? idx : kBuckets - 1;
  }

  static int64_t bucketUpperBound(int b) noexcept
  {
    if (b < kSub)
    {
      return b;
    }
    const int octave = b / kSub;
    const int sub = b % kSub;
    return (int64_t(1) << octave) + (int64_t(sub + 1) << (octave - kSubBits)) - 1;
  }

 private:
  std::atomic<uint64_t> _counts[kBuckets]{};
  std::atomic<uint64_t> _total{0};
  std::atomic<int64_t> _max{0};
};

}  // namespace flox::performance
