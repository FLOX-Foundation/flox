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
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace flox
{

// Client-side rate-limit model for SimulatedExecutor. Real venues
// enforce:
//   - per-IP weighted quotas (each endpoint costs `weight` units in
//     a sliding window)
//   - per-account order-action caps (e.g. 50 orders / 10s)
//   - burst bans after sustained 429s
//
// RateLimitPolicy mirrors that surface: add one or more buckets,
// optionally configure a ban-after-N-consecutive-rejects rule, then
// hand the policy to a SimulatedExecutor. The executor calls
// tryConsume(ActionKind, nowNs) before submit / cancel / replace and
// emits REJECTED_RATE_LIMIT when the call returns false.
class RateLimitPolicy
{
 public:
  enum class ActionKind : uint8_t
  {
    Submit = 0,
    Cancel = 1,
    Replace = 2,
  };

  // Add a sliding-window bucket. Per-action weight defaults are 1
  // for submit / cancel, 2 for replace (the typical venue weight).
  void addBucket(std::string name, int64_t windowNs, uint32_t capacity,
                 uint32_t submitWeight = 1, uint32_t cancelWeight = 1,
                 uint32_t replaceWeight = 2)
  {
    Bucket b{};
    b.name = std::move(name);
    b.windowNs = windowNs;
    b.capacity = capacity;
    b.submitWeight = submitWeight;
    b.cancelWeight = cancelWeight;
    b.replaceWeight = replaceWeight;
    _buckets.push_back(std::move(b));
  }

  // After N consecutive REJECTED_RATE_LIMIT outcomes, ban every
  // action for `banDurationNs`. Zero `after` disables the ban
  // mechanism.
  void setBan(uint32_t afterConsecutiveRejects, int64_t banDurationNs)
  {
    _banAfter = afterConsecutiveRejects;
    _banDurationNs = banDurationNs;
  }

  // Try to charge the action against every bucket at the given
  // timestamp. Returns true if every bucket accepts; false if any
  // bucket overflows or if a ban window is active.
  bool tryConsume(ActionKind action, int64_t nowNs)
  {
    if (_banUntilNs > nowNs)
    {
      ++_consecutiveRejects;
      return false;
    }
    // Compute per-bucket weight up front.
    uint32_t weight = 1;
    for (auto& b : _buckets)
    {
      const uint32_t w = pickWeight(b, action);
      // Evict expired entries.
      while (!b.consumed.empty() && b.consumed.front().first <= nowNs - b.windowNs)
      {
        b.used -= b.consumed.front().second;
        b.consumed.pop_front();
      }
      if (b.used + w > b.capacity)
      {
        // Reject. Do not modify any bucket — atomicity across buckets.
        ++_consecutiveRejects;
        maybeArmBan(nowNs);
        return false;
      }
      weight = w;  // not really; just to silence unused-variable
    }
    (void)weight;
    // Commit on every bucket.
    for (auto& b : _buckets)
    {
      const uint32_t w = pickWeight(b, action);
      b.consumed.emplace_back(nowNs, w);
      b.used += w;
    }
    _consecutiveRejects = 0;
    return true;
  }

  struct BucketState
  {
    std::string name;
    int64_t windowNs;
    uint32_t used;
    uint32_t capacity;
  };

  // Read per-bucket usage at `nowNs` (evicts expired entries first).
  std::vector<BucketState> bucketStates(int64_t nowNs)
  {
    std::vector<BucketState> out;
    out.reserve(_buckets.size());
    for (auto& b : _buckets)
    {
      while (!b.consumed.empty() && b.consumed.front().first <= nowNs - b.windowNs)
      {
        b.used -= b.consumed.front().second;
        b.consumed.pop_front();
      }
      out.push_back({b.name, b.windowNs, b.used, b.capacity});
    }
    return out;
  }

  int64_t banUntilNs() const noexcept { return _banUntilNs; }
  uint32_t consecutiveRejects() const noexcept { return _consecutiveRejects; }
  size_t bucketCount() const noexcept { return _buckets.size(); }

  // Canned profiles. Numbers approximate published rules; tune to
  // your account tier.
  static RateLimitPolicy binance_um_futures()
  {
    RateLimitPolicy p;
    // 50 orders / 10s rolling, 300 orders / 60s rolling.
    p.addBucket("orders_10s", 10'000'000'000LL, 50);
    p.addBucket("orders_60s", 60'000'000'000LL, 300);
    p.setBan(/*afterRejects=*/3, /*banNs=*/180'000'000'000LL);  // 3 min ban
    return p;
  }
  static RateLimitPolicy bybit_linear()
  {
    RateLimitPolicy p;
    // 10 orders/s + 100/min (approximate retail tier).
    p.addBucket("orders_1s", 1'000'000'000LL, 10);
    p.addBucket("orders_60s", 60'000'000'000LL, 100);
    p.setBan(5, 60'000'000'000LL);
    return p;
  }
  static RateLimitPolicy okx_swap()
  {
    RateLimitPolicy p;
    p.addBucket("orders_2s", 2'000'000'000LL, 60);
    p.setBan(3, 120'000'000'000LL);
    return p;
  }
  static RateLimitPolicy deribit()
  {
    RateLimitPolicy p;
    // 5 orders / 0.5s burst, 60 / 60s sustained.
    p.addBucket("orders_burst", 500'000'000LL, 5);
    p.addBucket("orders_60s", 60'000'000'000LL, 60);
    p.setBan(3, 60'000'000'000LL);
    return p;
  }

 private:
  struct Bucket
  {
    std::string name;
    int64_t windowNs;
    uint32_t capacity;
    uint32_t submitWeight;
    uint32_t cancelWeight;
    uint32_t replaceWeight;
    uint32_t used{0};
    std::deque<std::pair<int64_t, uint32_t>> consumed;
  };

  static uint32_t pickWeight(const Bucket& b, ActionKind a) noexcept
  {
    switch (a)
    {
      case ActionKind::Submit:
        return b.submitWeight;
      case ActionKind::Cancel:
        return b.cancelWeight;
      case ActionKind::Replace:
        return b.replaceWeight;
    }
    return 1;
  }

  void maybeArmBan(int64_t nowNs)
  {
    if (_banAfter > 0 && _consecutiveRejects >= _banAfter && _banDurationNs > 0)
    {
      _banUntilNs = nowNs + _banDurationNs;
      _consecutiveRejects = 0;
    }
  }

  std::vector<Bucket> _buckets;
  uint32_t _banAfter{0};
  int64_t _banDurationNs{0};
  int64_t _banUntilNs{0};
  uint32_t _consecutiveRejects{0};
};

}  // namespace flox
