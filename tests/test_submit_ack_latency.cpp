/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
void pushBook(SimulatedExecutor& exec, SymbolId sym, double bid, double bidQty,
              double ask, double askQty)
{
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(bid), Quantity::fromDouble(bidQty));
  asks.emplace_back(Price::fromDouble(ask), Quantity::fromDouble(askQty));
  exec.onBookUpdate(sym, bids, asks);
}

Order limitBuy(OrderId id, SymbolId sym, double price, double qty,
               TimeInForce tif = TimeInForce::GTC)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  o.timeInForce = tif;
  return o;
}

BacktestConfig configWithSubmitLatency(int64_t latencyNs, int64_t jitterNs = 0)
{
  BacktestConfig cfg{};
  cfg.queueModel = QueueModel::TOB;
  cfg.queueDepth = 1;
  cfg.submitAckLatencyNs = latencyNs;
  cfg.submitAckJitterNs = jitterNs;
  cfg.cancelAckSeed = 42;
  return cfg;
}
}  // namespace

TEST(SubmitAckLatency, ZeroLatencyKeepsSynchronousAccept)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithSubmitLatency(0));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));

  bool sawSubmitted = false;
  bool sawAccepted = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::SUBMITTED)
    {
      sawSubmitted = true;
    }
    if (ev.status == OrderEventStatus::ACCEPTED)
    {
      sawAccepted = true;
    }
  }
  EXPECT_TRUE(sawSubmitted);
  EXPECT_TRUE(sawAccepted);
}

TEST(SubmitAckLatency, AsyncAcceptDeferredUntilDeadline)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithSubmitLatency(10'000'000));  // 10 ms

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));

  // Immediately: SUBMITTED only.
  bool sawAcceptedEarly = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::ACCEPTED)
    {
      sawAcceptedEarly = true;
    }
  }
  EXPECT_FALSE(sawAcceptedEarly);

  // Advance past the ack deadline and feed a book tick.
  clock.advanceTo(clock.nowNs() + 11'000'000);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  bool sawAccepted = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::ACCEPTED)
    {
      sawAccepted = true;
    }
  }
  EXPECT_TRUE(sawAccepted);
}

TEST(SubmitAckLatency, LatePostOnlyCrossedRejectsWithReason)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithSubmitLatency(10'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // Place POST_ONLY buy at 100.5; ask at 101 — not crossing yet.
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.5, 1.0, TimeInForce::POST_ONLY));

  // Market moves: best ask drops to 100.0, below our 100.5 → would cross.
  clock.advanceTo(clock.nowNs() + 5'000'000);
  pushBook(exec, 1, 99.0, 5.0, 100.0, 5.0);

  // Advance past ack deadline and tick.
  clock.advanceTo(clock.nowNs() + 6'000'000);
  pushBook(exec, 1, 99.0, 5.0, 100.0, 5.0);

  bool sawAccepted = false;
  bool sawRejected = false;
  std::string reason;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::ACCEPTED)
    {
      sawAccepted = true;
    }
    if (ev.status == OrderEventStatus::REJECTED)
    {
      sawRejected = true;
      reason = ev.rejectReason;
    }
  }
  EXPECT_TRUE(sawAccepted);
  EXPECT_TRUE(sawRejected);
  EXPECT_EQ(reason, "late_post_only_crossed");
}

TEST(SubmitAckLatency, OrderDoesNotFillUntilAccepted)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithSubmitLatency(10'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // Limit buy alone at 100 (queue tracker registers it on accept).
  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));

  // Aggressive sell at 100 arrives BEFORE the ack deadline. Should not
  // fill our order — it is not yet in the queue tracker.
  clock.advanceTo(clock.nowNs() + 1'000'000);
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(2.0), false);

  bool sawFilledEarly = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED)
    {
      sawFilledEarly = true;
    }
  }
  EXPECT_FALSE(sawFilledEarly);
}