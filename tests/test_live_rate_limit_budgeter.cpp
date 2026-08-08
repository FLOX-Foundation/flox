/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <vector>

#include "flox/execution/live_rate_limit_budgeter.h"
#include "flox/execution/rate_limit_policy.h"

using flox::LiveRateLimitBudgeter;
using flox::RateLimitPolicy;
using Action = RateLimitPolicy::ActionKind;
using Decision = LiveRateLimitBudgeter::Decision;

namespace
{

RateLimitPolicy tinyPolicy(uint32_t capacity)
{
  RateLimitPolicy p;
  p.addBucket("orders", 1'000'000'000LL /*1s*/, capacity);
  return p;
}

}  // namespace

TEST(LiveRateLimitBudgeter, AllowsWithinBudget)
{
  LiveRateLimitBudgeter b(tinyPolicy(10), {.cancelHeadroom = 0.0});
  for (int i = 0; i < 10; ++i)
  {
    EXPECT_EQ(b.tryAcquire(1, Action::Submit, 100), Decision::ALLOW);
  }
  EXPECT_EQ(b.tryAcquire(1, Action::Submit, 100), Decision::REJECT_BUDGET);
  EXPECT_EQ(b.metrics().allowed, 10u);
  EXPECT_EQ(b.metrics().rejectedBudget, 1u);
}

TEST(LiveRateLimitBudgeter, CancelHeadroomKeepsCancelsAliveWhenSubmitsAreRefused)
{
  // Capacity 10, headroom 20%: submits see 8, cancels see 10.
  LiveRateLimitBudgeter b(tinyPolicy(10), {.cancelHeadroom = 0.2});

  int submitsAllowed = 0;
  while (b.tryAcquire(1, Action::Submit, 100) == Decision::ALLOW)
  {
    ++submitsAllowed;
  }
  EXPECT_EQ(submitsAllowed, 8);

  // The refusal was the reserve, not the bucket.
  EXPECT_EQ(b.metrics().rejectedHeadroom, 1u);
  EXPECT_EQ(b.metrics().rejectedBudget, 0u);

  // Cancels still pass -- the reserved slice belongs to them.
  EXPECT_EQ(b.tryAcquire(1, Action::Cancel, 101), Decision::ALLOW);
  EXPECT_EQ(b.tryAcquire(1, Action::Cancel, 102), Decision::ALLOW);
  // Bucket is now truly full for everyone.
  EXPECT_EQ(b.tryAcquire(1, Action::Cancel, 103), Decision::REJECT_BUDGET);
}

TEST(LiveRateLimitBudgeter, WindowSlidesAndBudgetRecovers)
{
  LiveRateLimitBudgeter b(tinyPolicy(2), {.cancelHeadroom = 0.0});
  EXPECT_EQ(b.tryAcquire(1, Action::Submit, 0), Decision::ALLOW);
  EXPECT_EQ(b.tryAcquire(1, Action::Submit, 0), Decision::ALLOW);
  EXPECT_EQ(b.tryAcquire(1, Action::Submit, 1), Decision::REJECT_BUDGET);
  // One second later the window is clear.
  EXPECT_EQ(b.tryAcquire(1, Action::Submit, 1'000'000'001LL), Decision::ALLOW);
}

TEST(LiveRateLimitBudgeter, PerStrategyQuotaIsolatesGreedyStrategy)
{
  LiveRateLimitBudgeter b(tinyPolicy(100), {.cancelHeadroom = 0.0});
  b.setStrategyQuota(/*strategyId=*/1, 1'000'000'000LL, /*capacity=*/3);

  EXPECT_EQ(b.tryAcquire(1, Action::Submit, 10), Decision::ALLOW);
  EXPECT_EQ(b.tryAcquire(1, Action::Submit, 11), Decision::ALLOW);
  EXPECT_EQ(b.tryAcquire(1, Action::Submit, 12), Decision::ALLOW);
  EXPECT_EQ(b.tryAcquire(1, Action::Submit, 13), Decision::REJECT_STRATEGY);

  // Другая стратегия живёт на общем бюджете свободно.
  EXPECT_EQ(b.tryAcquire(2, Action::Submit, 14), Decision::ALLOW);

  // Cancels bypass the strategy quota by default.
  EXPECT_EQ(b.tryAcquire(1, Action::Cancel, 15), Decision::ALLOW);
}

TEST(LiveRateLimitBudgeter, SharedProfileMatchesBacktestSemantics)
{
  // The same canned venue profile drives both the backtest SimulatedExecutor
  // and the live budgeter: with zero headroom the live decision must match
  // the raw policy decision action for action.
  auto liveProfile = RateLimitPolicy::deribit();
  auto backtestProfile = RateLimitPolicy::deribit();

  LiveRateLimitBudgeter live(std::move(liveProfile), {.cancelHeadroom = 0.0});

  for (int i = 0; i < 20; ++i)
  {
    const int64_t now = 1'000'000LL * i;
    const bool backtestAllows = backtestProfile.tryConsume(Action::Submit, now);
    const bool liveAllows = live.tryAcquire(7, Action::Submit, now) == Decision::ALLOW;
    EXPECT_EQ(backtestAllows, liveAllows) << "diverged at action " << i;
  }
}

namespace
{

class RecordingExecutor : public flox::IRoutableExecutor
{
 public:
  void submit(flox::SymbolId, flox::Side, int64_t, int64_t, flox::OrderId id) override
  {
    submitted.push_back(id);
  }
  void cancel(flox::OrderId id) override { canceled.push_back(id); }

  std::vector<flox::OrderId> submitted;
  std::vector<flox::OrderId> canceled;
};

int64_t fakeClock() { return 500; }

struct RejectLog
{
  std::vector<std::pair<flox::OrderId, Decision>> entries;
};

void onReject(flox::OrderId id, Decision d, void* user)
{
  static_cast<RejectLog*>(user)->entries.emplace_back(id, d);
}

}  // namespace

TEST(RateLimitedExecutor, DecoratorDropsLocallyAndReports)
{
  LiveRateLimitBudgeter budgeter(tinyPolicy(2), {.cancelHeadroom = 0.0});
  RecordingExecutor inner;
  RejectLog log;
  flox::RateLimitedExecutor exec(&inner, &budgeter, /*strategyId=*/1, &fakeClock,
                                 &onReject, &log);

  exec.submit(1, flox::Side::BUY, 100, 1, /*orderId=*/11);
  exec.submit(1, flox::Side::BUY, 100, 1, /*orderId=*/12);
  exec.submit(1, flox::Side::BUY, 100, 1, /*orderId=*/13);  // over budget

  ASSERT_EQ(inner.submitted.size(), 2u);
  ASSERT_EQ(log.entries.size(), 1u);
  EXPECT_EQ(log.entries[0].first, 13u);
  EXPECT_EQ(log.entries[0].second, Decision::REJECT_BUDGET);
}
