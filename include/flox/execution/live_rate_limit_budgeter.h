/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include "flox/execution/order_router.h"
#include "flox/execution/rate_limit_policy.h"
#include "flox/log/log.h"

namespace flox
{

// Live-side rate-limit budgeter: venue quotas as a first-class resource
// instead of a source of surprise rejects. Shares the quota core
// (RateLimitPolicy, same buckets and venue profiles) with the backtest
// SimulatedExecutor -- one mechanism, so a strategy that fits the budget in
// backtest fits it live.
//
// Adds the concerns the backtest model does not need:
//   - cancel headroom: a slice of every trading bucket is reserved for
//     cancels. A market maker that cannot cancel is losing money; submits
//     are cheaper to refuse locally than cancels are to delay.
//   - per-strategy quotas: one greedy strategy cannot exhaust the account
//     budget for everyone.
//   - local reject with metrics: refusing locally is strictly better than a
//     venue 429 -- the venue counts rejects toward bans.
class LiveRateLimitBudgeter
{
 public:
  using Action = RateLimitPolicy::ActionKind;

  enum class Decision : uint8_t
  {
    ALLOW,
    REJECT_BUDGET,    // venue-level bucket exhausted
    REJECT_HEADROOM,  // submit/replace hit the cancel reserve
    REJECT_STRATEGY   // per-strategy quota exhausted
  };

  struct Config
  {
    // Fraction of every trading bucket visible only to cancels.
    double cancelHeadroom{0.15};
    // Cancels skip per-strategy quotas: pulling risk always allowed.
    bool cancelsBypassStrategyQuota{true};
  };

  explicit LiveRateLimitBudgeter(RateLimitPolicy policy)
      : _policy(std::move(policy)), _cfg(Config{})
  {
  }

  LiveRateLimitBudgeter(RateLimitPolicy policy, Config cfg)
      : _policy(std::move(policy)), _cfg(cfg)
  {
  }

  // Absolute per-strategy sliding-window quota on trading actions.
  void setStrategyQuota(uint32_t strategyId, int64_t windowNs, uint32_t capacity)
  {
    _strategyQuota[strategyId] = StrategyQuota{windowNs, capacity, 0, {}};
  }

  Decision tryAcquire(uint32_t strategyId, Action action, int64_t nowNs)
  {
    const bool isCancel = action == Action::Cancel;

    if (!isCancel || !_cfg.cancelsBypassStrategyQuota)
    {
      if (auto it = _strategyQuota.find(strategyId); it != _strategyQuota.end())
      {
        auto& q = it->second;
        evict(q, nowNs);
        if (q.used + 1 > q.capacity)
        {
          ++_metrics.rejectedStrategy;
          return Decision::REJECT_STRATEGY;
        }
      }
    }

    const double scale = isCancel ? 1.0 : 1.0 - _cfg.cancelHeadroom;
    if (!_policy.tryConsume(action, nowNs, scale))
    {
      // Distinguish "the bucket is truly full" from "the submit hit the
      // cancel reserve": a full-capacity probe that would pass means the
      // reserve was the reason. Probe without committing: ban/reject
      // bookkeeping inside the policy already counted this attempt.
      if (!isCancel && wouldPassAtFullCapacity(action, nowNs))
      {
        ++_metrics.rejectedHeadroom;
        return Decision::REJECT_HEADROOM;
      }
      ++_metrics.rejectedBudget;
      return Decision::REJECT_BUDGET;
    }

    if (!isCancel || !_cfg.cancelsBypassStrategyQuota)
    {
      if (auto it = _strategyQuota.find(strategyId); it != _strategyQuota.end())
      {
        auto& q = it->second;
        q.consumed.emplace_back(nowNs);
        ++q.used;
      }
    }

    ++_metrics.allowed;
    return Decision::ALLOW;
  }

  struct Metrics
  {
    uint64_t allowed{0};
    uint64_t rejectedBudget{0};
    uint64_t rejectedHeadroom{0};
    uint64_t rejectedStrategy{0};
  };

  const Metrics& metrics() const noexcept { return _metrics; }

  std::vector<RateLimitPolicy::BucketState> bucketStates(int64_t nowNs)
  {
    return _policy.bucketStates(nowNs);
  }

  RateLimitPolicy& policy() noexcept { return _policy; }

 private:
  struct StrategyQuota
  {
    int64_t windowNs{0};
    uint32_t capacity{0};
    uint32_t used{0};
    std::deque<int64_t> consumed;
  };

  static void evict(StrategyQuota& q, int64_t nowNs)
  {
    while (!q.consumed.empty() && q.consumed.front() <= nowNs - q.windowNs)
    {
      q.consumed.pop_front();
      --q.used;
    }
  }

  bool wouldPassAtFullCapacity(Action action, int64_t nowNs)
  {
    // bucketStates() evicts expired entries, then we re-check the charge
    // against the full (unscaled) capacity.
    const auto states = _policy.bucketStates(nowNs);
    for (const auto& st : states)
    {
      if (st.endpointFamily != RateLimitPolicy::familyOf(action))
      {
        continue;
      }
      if (st.used + 1 > st.capacity)
      {
        return false;
      }
    }
    return true;
  }

  RateLimitPolicy _policy;
  Config _cfg;
  Metrics _metrics;
  std::unordered_map<uint32_t, StrategyQuota> _strategyQuota;
};

// IRoutableExecutor decorator: place between the OrderRouter and a venue
// executor to enforce the budget at the routing boundary. Rejected actions
// invoke the callback and never reach the wire.
class RateLimitedExecutor : public IRoutableExecutor
{
 public:
  using RejectCallback = void (*)(OrderId, LiveRateLimitBudgeter::Decision, void* user);
  using ClockFn = int64_t (*)();

  RateLimitedExecutor(IRoutableExecutor* inner, LiveRateLimitBudgeter* budgeter,
                      uint32_t strategyId, ClockFn clock,
                      RejectCallback onReject = nullptr, void* user = nullptr)
      : _inner(inner),
        _budgeter(budgeter),
        _strategyId(strategyId),
        _clock(clock),
        _onReject(onReject),
        _user(user)
  {
  }

  void submit(SymbolId symbol, Side side, int64_t priceRaw, int64_t quantityRaw,
              OrderId orderId) override
  {
    const auto d = _budgeter->tryAcquire(_strategyId, LiveRateLimitBudgeter::Action::Submit,
                                         _clock());
    if (d != LiveRateLimitBudgeter::Decision::ALLOW)
    {
      reject(orderId, d);
      return;
    }
    _inner->submit(symbol, side, priceRaw, quantityRaw, orderId);
  }

  void cancel(OrderId orderId) override
  {
    const auto d = _budgeter->tryAcquire(_strategyId, LiveRateLimitBudgeter::Action::Cancel,
                                         _clock());
    if (d != LiveRateLimitBudgeter::Decision::ALLOW)
    {
      // A refused cancel is an incident, not a statistic.
      FLOX_LOG_ERROR("RateLimitedExecutor: cancel for order " << orderId
                                                              << " refused by budget");
      reject(orderId, d);
      return;
    }
    _inner->cancel(orderId);
  }

 private:
  void reject(OrderId orderId, LiveRateLimitBudgeter::Decision d)
  {
    if (_onReject)
    {
      _onReject(orderId, d, _user);
    }
  }

  IRoutableExecutor* _inner;
  LiveRateLimitBudgeter* _budgeter;
  uint32_t _strategyId;
  ClockFn _clock;
  RejectCallback _onReject;
  void* _user;
};

}  // namespace flox
