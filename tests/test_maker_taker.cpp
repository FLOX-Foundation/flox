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
}  // namespace

TEST(MakerTaker, ResterFilledFromQueueConsumptionIsMaker)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // Order rests alone at 100 (queue-ahead = 0).
  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 2.0));

  // Aggressive sell at 100 walks into our resting order.
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(2.0), false);

  bool sawFill = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED)
    {
      sawFill = true;
      EXPECT_TRUE(ev.isMaker);
    }
  }
  EXPECT_TRUE(sawFill);
}

TEST(MakerTaker, MarketOrderIsTaker)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(marketBuy(2, 1, 1.0));

  bool sawFill = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED)
    {
      sawFill = true;
      EXPECT_FALSE(ev.isMaker);
    }
  }
  EXPECT_TRUE(sawFill);
}

TEST(MakerTaker, MarketableLimitCrossingIsTaker)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // Best ask sits at 101; a buy limit at 102 is marketable on
  // arrival and crosses the book.
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(3, 1, 102.0, 1.0));

  bool sawFill = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED || ev.status == OrderEventStatus::PARTIALLY_FILLED)
    {
      sawFill = true;
      EXPECT_FALSE(ev.isMaker);
    }
  }
  EXPECT_TRUE(sawFill);
}

TEST(MakerTaker, NonFillStatusesCarryFalseIsMaker)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(7, 1, 99.0, 1.0));
  exec.cancelOrder(7);

  for (const auto& ev : events)
  {
    if (ev.status != OrderEventStatus::PARTIALLY_FILLED &&
        ev.status != OrderEventStatus::FILLED)
    {
      EXPECT_FALSE(ev.isMaker);
    }
  }
}
