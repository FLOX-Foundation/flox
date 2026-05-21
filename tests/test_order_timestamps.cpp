/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
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

Order marketBuy(OrderId id, SymbolId sym, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

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
}  // namespace

TEST(OrderTimestamps, SubmittedAcceptedStampedOnLimit)
{
  SimulatedClock clock;
  clock.advanceTo(UnixNanos(1'000'000));
  SimulatedExecutor exec(clock);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));

  OrderEvent submitted{};
  OrderEvent accepted{};
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::SUBMITTED)
    {
      submitted = ev;
    }
    if (ev.status == OrderEventStatus::ACCEPTED)
    {
      accepted = ev;
    }
  }
  EXPECT_NE(submitted.timestamps.submittedAtNs, 0);
  EXPECT_NE(accepted.timestamps.submittedAtNs, 0);
  EXPECT_NE(accepted.timestamps.acceptedAtNs, 0);
  // ack timestamp is at least as large as submit (same clock tick in this
  // sim is fine since they fire back-to-back).
  EXPECT_GE(accepted.timestamps.acceptedAtNs,
            accepted.timestamps.submittedAtNs);
  // Stages not reached yet read zero.
  EXPECT_EQ(accepted.timestamps.firstFillAtNs, 0);
  EXPECT_EQ(accepted.timestamps.canceledAtNs, 0);
}

TEST(OrderTimestamps, FirstAndLastFillStampedOnPartialFlow)
{
  SimulatedClock clock;
  clock.advanceTo(UnixNanos(1000));
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 3.0));

  clock.advanceTo(clock.nowNs() + 10);
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(2.0), false);
  clock.advanceTo(clock.nowNs() + 10);
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);

  OrderEvent firstFill{};
  OrderEvent lastFill{};
  bool sawFirst = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::PARTIALLY_FILLED ||
        ev.status == OrderEventStatus::FILLED)
    {
      if (!sawFirst)
      {
        firstFill = ev;
        sawFirst = true;
      }
      lastFill = ev;
    }
  }
  ASSERT_TRUE(sawFirst);
  EXPECT_NE(firstFill.timestamps.firstFillAtNs, 0);
  EXPECT_EQ(firstFill.timestamps.firstFillAtNs,
            firstFill.timestamps.lastFillAtNs);
  // Last fill keeps the original first-fill timestamp but updates lastFill.
  EXPECT_EQ(lastFill.timestamps.firstFillAtNs,
            firstFill.timestamps.firstFillAtNs);
  EXPECT_GT(lastFill.timestamps.lastFillAtNs,
            firstFill.timestamps.lastFillAtNs);
}

TEST(OrderTimestamps, CanceledAtStampedOnCancel)
{
  SimulatedClock clock;
  clock.advanceTo(UnixNanos(100));
  SimulatedExecutor exec(clock);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(42, 1, 99.0, 1.0));
  clock.advanceTo(clock.nowNs() + 50);
  exec.cancelOrder(42);

  bool sawCancel = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::CANCELED && ev.order.id == 42)
    {
      sawCancel = true;
      EXPECT_NE(ev.timestamps.canceledAtNs, 0);
      EXPECT_NE(ev.timestamps.submittedAtNs, 0);
      EXPECT_GT(ev.timestamps.canceledAtNs, ev.timestamps.submittedAtNs);
    }
  }
  EXPECT_TRUE(sawCancel);
}

TEST(OrderTimestamps, MarketOrderFillStampsFirstFillImmediately)
{
  SimulatedClock clock;
  clock.advanceTo(UnixNanos(500));
  SimulatedExecutor exec(clock);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(marketBuy(7, 1, 1.0));

  bool sawFill = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED && ev.order.id == 7)
    {
      sawFill = true;
      EXPECT_NE(ev.timestamps.firstFillAtNs, 0);
      EXPECT_EQ(ev.timestamps.firstFillAtNs, ev.timestamps.lastFillAtNs);
    }
  }
  EXPECT_TRUE(sawFill);
}
