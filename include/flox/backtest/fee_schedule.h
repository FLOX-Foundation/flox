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
#include <utility>
#include <vector>

namespace flox
{

// One tier in a venue's volume-based fee ladder. `minNotional30d` is
// the inclusive lower bound on 30-day rolling notional that qualifies
// for this tier. Both maker and taker rates are expressed in basis
// points (1 bp = 0.01%). A negative maker rate represents a rebate
// — the account *receives* the rate per maker fill.
struct FeeTier
{
  double minNotional30d{0.0};
  double makerBps{0.0};
  double takerBps{0.0};
};

// Tiered maker/taker fee schedule. Models the volume tiers that real
// venues use (Binance UM has 10 tiers, Bybit 7, OKX 5, Deribit 4).
//
// Usage pattern:
//
//   FeeSchedule sched = FeeSchedule::binance_um_futures();
//   for (each fill) {
//     double fee = sched.feeFor(ts_ns, notional, is_maker);
//     equity -= fee;            // positive = paid, negative = rebate received
//     sched.recordFill(ts_ns, notional);
//   }
//
// `recordFill` pushes the notional into a 30-day rolling window; the
// next `feeFor` / `currentTierIndex` call resolves the active tier
// from the rolling sum.
class FeeSchedule
{
 public:
  static constexpr int64_t kThirtyDaysNs = 30LL * 24LL * 3600LL * 1'000'000'000LL;

  FeeSchedule() = default;

  // Tiers are inserted unordered; the schedule sorts them by
  // `minNotional30d` ascending. If two tiers share the same min,
  // the latest insertion wins.
  void addTier(double minNotional30d, double makerBps, double takerBps)
  {
    _tiers.push_back({minNotional30d, makerBps, takerBps});
    std::sort(_tiers.begin(), _tiers.end(),
              [](const FeeTier& a, const FeeTier& b)
              { return a.minNotional30d < b.minNotional30d; });
  }

  // Note: notional is unsigned. Sum into the 30-day rolling window.
  void recordFill(int64_t tsNs, double notional)
  {
    _rolling.emplace_back(tsNs, notional);
    _rollingTotal += notional;
    evictExpired(tsNs);
    const size_t newTier = resolveTierIndex();
    if (_tiers.empty())
    {
      _currentTier = 0;
      return;
    }
    if (newTier != _currentTier)
    {
      _tierTransitions.push_back(tsNs);
      _currentTier = newTier;
    }
  }

  // Look up the (makerBps, takerBps) pair for the tier active at
  // `nowNs`. Evicts expired entries first.
  std::pair<double, double> currentBps(int64_t nowNs)
  {
    evictExpired(nowNs);
    size_t idx = resolveTierIndex();
    if (_tiers.empty())
    {
      return {0.0, 0.0};
    }
    return {_tiers[idx].makerBps, _tiers[idx].takerBps};
  }

  // Fee in margin currency for a fill of `notional` at `tsNs`. Sign
  // convention: positive value means the account paid the fee;
  // negative means a rebate was received (maker negative-rate tier).
  double feeFor(int64_t tsNs, double notional, bool isMaker)
  {
    auto [maker, taker] = currentBps(tsNs);
    const double bps = isMaker ? maker : taker;
    return notional * bps * 1e-4;
  }

  size_t currentTierIndex() const noexcept { return _currentTier; }
  size_t tierCount() const noexcept { return _tiers.size(); }
  const std::vector<FeeTier>& tiers() const noexcept { return _tiers; }
  const std::vector<int64_t>& tierTransitionTsNs() const noexcept
  {
    return _tierTransitions;
  }
  double rollingNotional30d() const noexcept { return _rollingTotal; }

  // Canned profiles. Numbers approximate published rules; tune for
  // your actual VIP tier. Maker negative = rebate received.

  static FeeSchedule binance_um_futures()
  {
    // 10-tier ladder, simplified to the published VIP brackets.
    // 30-day rolling notional in USD.
    FeeSchedule s;
    s.addTier(0, 2.0, 4.0);               // Regular
    s.addTier(250'000, 1.6, 4.0);         // VIP 1
    s.addTier(2'500'000, 1.4, 3.5);       // VIP 2
    s.addTier(7'500'000, 1.2, 3.2);       // VIP 3
    s.addTier(22'500'000, 1.0, 3.0);      // VIP 4
    s.addTier(50'000'000, 0.8, 2.7);      // VIP 5
    s.addTier(100'000'000, 0.6, 2.5);     // VIP 6
    s.addTier(200'000'000, 0.4, 2.3);     // VIP 7
    s.addTier(400'000'000, 0.2, 2.1);     // VIP 8
    s.addTier(1'000'000'000, -0.5, 1.7);  // VIP 9 — maker rebate
    return s;
  }

  static FeeSchedule bybit_linear()
  {
    FeeSchedule s;
    s.addTier(0, 1.0, 6.0);             // VIP 0
    s.addTier(5'000'000, 0.8, 5.0);     // VIP 1
    s.addTier(25'000'000, 0.5, 4.0);    // VIP 2
    s.addTier(50'000'000, 0.2, 3.5);    // VIP 3
    s.addTier(100'000'000, 0.0, 3.0);   // VIP 4
    s.addTier(250'000'000, -0.5, 2.5);  // VIP 5 — maker rebate
    return s;
  }

  static FeeSchedule okx_swap()
  {
    FeeSchedule s;
    s.addTier(0, 2.0, 5.0);
    s.addTier(5'000'000, 1.5, 4.5);
    s.addTier(25'000'000, 1.0, 4.0);
    s.addTier(100'000'000, 0.0, 3.5);
    return s;
  }

  static FeeSchedule deribit()
  {
    // Deribit futures/perps default to maker rebate above LV-1.
    FeeSchedule s;
    s.addTier(0, 0.0, 5.0);           // Default
    s.addTier(1'000'000, -1.0, 5.0);  // LV 1+ — maker rebate
    return s;
  }

  // Reset rolling state and tier-transition log (keeps tier list).
  void resetRolling() noexcept
  {
    _rolling.clear();
    _rollingTotal = 0.0;
    _currentTier = 0;
    _tierTransitions.clear();
  }

 private:
  size_t resolveTierIndex() const
  {
    if (_tiers.empty())
    {
      return 0;
    }
    size_t idx = 0;
    for (size_t i = 0; i < _tiers.size(); ++i)
    {
      if (_rollingTotal >= _tiers[i].minNotional30d)
      {
        idx = i;
      }
      else
      {
        break;
      }
    }
    return idx;
  }

  void evictExpired(int64_t nowNs)
  {
    const int64_t cutoff = nowNs - kThirtyDaysNs;
    while (!_rolling.empty() && _rolling.front().first <= cutoff)
    {
      _rollingTotal -= _rolling.front().second;
      _rolling.pop_front();
    }
    if (_rollingTotal < 0.0)
    {
      _rollingTotal = 0.0;
    }
  }

  std::vector<FeeTier> _tiers;
  std::deque<std::pair<int64_t, double>> _rolling;
  double _rollingTotal{0.0};
  size_t _currentTier{0};
  std::vector<int64_t> _tierTransitions;
};

}  // namespace flox
