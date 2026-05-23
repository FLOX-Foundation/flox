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
  // Endpoint families that real venues budget separately. A bucket
  // belongs to exactly one family; tryConsume only charges buckets
  // whose family matches the action's family. Default everywhere is
  // Trading, so a single-bucket policy keeps T022 semantics.
  enum class EndpointFamily : uint8_t
  {
    Trading = 0,
    MarketData = 1,
    Account = 2,
  };

  enum class ActionKind : uint8_t
  {
    Submit = 0,
    Cancel = 1,
    Replace = 2,
    QueryAccount = 3,
    QueryMarketData = 4,
  };

  // Add a sliding-window bucket. Per-action weight defaults are 1
  // for submit / cancel, 2 for replace (the typical venue weight).
  // Bucket defaults to the Trading family (matches T022 semantics).
  void addBucket(std::string name, int64_t windowNs, uint32_t capacity,
                 uint32_t submitWeight = 1, uint32_t cancelWeight = 1,
                 uint32_t replaceWeight = 2,
                 EndpointFamily family = EndpointFamily::Trading,
                 uint32_t queryWeight = 1)
  {
    Bucket b{};
    b.name = std::move(name);
    b.endpointFamily = family;
    b.windowNs = windowNs;
    b.capacity = capacity;
    b.submitWeight = submitWeight;
    b.cancelWeight = cancelWeight;
    b.replaceWeight = replaceWeight;
    b.queryWeight = queryWeight;
    _buckets.push_back(std::move(b));
  }

  // Convenience for non-Trading families: only the query weight is
  // meaningful, so don't force callers to specify the trading weights.
  void addFamilyBucket(EndpointFamily family, std::string name, int64_t windowNs,
                       uint32_t capacity, uint32_t queryWeight = 1)
  {
    addBucket(std::move(name), windowNs, capacity, 1, 1, 2, family, queryWeight);
  }

  // Endpoint family that an action consumes from.
  static EndpointFamily familyOf(ActionKind action) noexcept
  {
    switch (action)
    {
      case ActionKind::Submit:
      case ActionKind::Cancel:
      case ActionKind::Replace:
        return EndpointFamily::Trading;
      case ActionKind::QueryAccount:
        return EndpointFamily::Account;
      case ActionKind::QueryMarketData:
        return EndpointFamily::MarketData;
    }
    return EndpointFamily::Trading;
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
    const EndpointFamily family = familyOf(action);
    // Check every matching bucket atomically — we only commit if all
    // of them accept the charge.
    for (auto& b : _buckets)
    {
      if (b.endpointFamily != family)
      {
        continue;
      }
      const uint32_t w = pickWeight(b, action);
      while (!b.consumed.empty() && b.consumed.front().first <= nowNs - b.windowNs)
      {
        b.used -= b.consumed.front().second;
        b.consumed.pop_front();
      }
      if (b.used + w > b.capacity)
      {
        ++_consecutiveRejects;
        maybeArmBan(nowNs);
        return false;
      }
    }
    // Commit on every matching bucket.
    for (auto& b : _buckets)
    {
      if (b.endpointFamily != family)
      {
        continue;
      }
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
    EndpointFamily endpointFamily{EndpointFamily::Trading};
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
      out.push_back({b.name, b.endpointFamily, b.windowNs, b.used, b.capacity});
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
    // Trading pool: 50 orders / 10s, 300 orders / 60s.
    p.addBucket("orders_10s", 10'000'000'000LL, 50);
    p.addBucket("orders_60s", 60'000'000'000LL, 300);
    // Market-data pool: ~6000 weight / min (independent of trading).
    p.addFamilyBucket(EndpointFamily::MarketData, "market_data_60s",
                      60'000'000'000LL, 6000);
    // Account pool: ~1200 weight / min for position/balance queries.
    p.addFamilyBucket(EndpointFamily::Account, "account_60s",
                      60'000'000'000LL, 1200);
    p.setBan(/*afterRejects=*/3, /*banNs=*/180'000'000'000LL);  // 3 min ban
    return p;
  }
  static RateLimitPolicy bybit_linear()
  {
    RateLimitPolicy p;
    // 10 orders/s + 100/min (approximate retail tier).
    p.addBucket("orders_1s", 1'000'000'000LL, 10);
    p.addBucket("orders_60s", 60'000'000'000LL, 100);
    p.addFamilyBucket(EndpointFamily::MarketData, "market_data_60s",
                      60'000'000'000LL, 1200);
    p.addFamilyBucket(EndpointFamily::Account, "account_60s",
                      60'000'000'000LL, 600);
    p.setBan(5, 60'000'000'000LL);
    return p;
  }
  static RateLimitPolicy okx_swap()
  {
    RateLimitPolicy p;
    p.addBucket("orders_2s", 2'000'000'000LL, 60);
    p.addFamilyBucket(EndpointFamily::MarketData, "market_data_2s",
                      2'000'000'000LL, 40);
    p.addFamilyBucket(EndpointFamily::Account, "account_2s",
                      2'000'000'000LL, 20);
    p.setBan(3, 120'000'000'000LL);
    return p;
  }
  static RateLimitPolicy deribit()
  {
    RateLimitPolicy p;
    // 5 orders / 0.5s burst, 60 / 60s sustained.
    p.addBucket("orders_burst", 500'000'000LL, 5);
    p.addBucket("orders_60s", 60'000'000'000LL, 60);
    p.addFamilyBucket(EndpointFamily::MarketData, "market_data_60s",
                      60'000'000'000LL, 500);
    p.addFamilyBucket(EndpointFamily::Account, "account_60s",
                      60'000'000'000LL, 120);
    p.setBan(3, 60'000'000'000LL);
    return p;
  }

 private:
  struct Bucket
  {
    std::string name;
    EndpointFamily endpointFamily{EndpointFamily::Trading};
    int64_t windowNs;
    uint32_t capacity;
    uint32_t submitWeight;
    uint32_t cancelWeight;
    uint32_t replaceWeight;
    uint32_t queryWeight{1};
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
      case ActionKind::QueryAccount:
      case ActionKind::QueryMarketData:
        return b.queryWeight;
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
