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

Order limitBuy(OrderId id, SymbolId sym, double price, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

BacktestConfig configWithReplaceLatency(int64_t latencyNs, int64_t jitterNs = 0)
{
  BacktestConfig cfg{};
  cfg.queueModel = QueueModel::TOB;
  cfg.queueDepth = 1;
  cfg.replaceAckLatencyNs = latencyNs;
  cfg.replaceAckJitterNs = jitterNs;
  cfg.cancelAckSeed = 42;
  return cfg;
}
}  // namespace

TEST(ReplaceInFlight, ZeroLatencyKeepsLegacyBehavior)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(0));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  Order replacement = limitBuy(1, 1, 99.5, 1.0);
  exec.replaceOrder(1, replacement);

  bool sawReplaced = false;
  bool sawReplaceSubmitted = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::REPLACE_SUBMITTED)
    {
      sawReplaceSubmitted = true;
    }
    if (ev.status == OrderEventStatus::REPLACED)
    {
      sawReplaced = true;
    }
  }
  EXPECT_TRUE(sawReplaced);
  EXPECT_FALSE(sawReplaceSubmitted);
}

TEST(ReplaceInFlight, AsyncReplaceFiresSubmittedThenAcceptedAndReplaced)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(10'000'000));  // 10 ms

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  Order replacement = limitBuy(1, 1, 99.5, 1.0);
  exec.replaceOrder(1, replacement);

  // Immediately: REPLACE_SUBMITTED only.
  bool sawSubmitted = false;
  bool sawAcceptedEarly = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::REPLACE_SUBMITTED)
    {
      sawSubmitted = true;
    }
    if (ev.status == OrderEventStatus::REPLACE_ACCEPTED)
    {
      sawAcceptedEarly = true;
    }
  }
  EXPECT_TRUE(sawSubmitted);
  EXPECT_FALSE(sawAcceptedEarly);

  clock.advanceTo(clock.nowNs() + 11'000'000);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  bool sawAccepted = false;
  bool sawReplaced = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::REPLACE_ACCEPTED)
    {
      sawAccepted = true;
    }
    if (ev.status == OrderEventStatus::REPLACED)
    {
      sawReplaced = true;
    }
  }
  EXPECT_TRUE(sawAccepted);
  EXPECT_TRUE(sawReplaced);
}

TEST(ReplaceInFlight, LateReplaceAfterFillEmitsRejected)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(10'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  Order replacement = limitBuy(1, 1, 99.5, 1.0);
  exec.replaceOrder(1, replacement);

  // Trade arrives within the 10 ms window and fills the original order.
  clock.advanceTo(clock.nowNs() + 1'000'000);
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);

  bool sawFilled = false;
  bool sawReject = false;
  std::string reason;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED)
    {
      sawFilled = true;
    }
    if (ev.status == OrderEventStatus::REPLACE_REJECTED)
    {
      sawReject = true;
      reason = ev.rejectReason;
    }
  }
  EXPECT_TRUE(sawFilled);
  EXPECT_TRUE(sawReject);
  EXPECT_EQ(reason, "late_replace_after_fill");
}

TEST(ReplaceInFlight, AckCarriesNewOrderPayload)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(5'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  Order replacement = limitBuy(1, 1, 99.5, 2.0);
  exec.replaceOrder(1, replacement);

  clock.advanceTo(clock.nowNs() + 6'000'000);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::REPLACE_ACCEPTED)
    {
      EXPECT_DOUBLE_EQ(ev.newOrder.price.toDouble(), 99.5);
      EXPECT_DOUBLE_EQ(ev.newOrder.quantity.toDouble(), 2.0);
    }
  }
}
