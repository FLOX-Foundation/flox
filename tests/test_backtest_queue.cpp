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

Order limitSell(OrderId id, SymbolId sym, double price, double qty)
{
  Order o = limitBuy(id, sym, price, qty);
  o.side = Side::SELL;
  return o;
}
}  // namespace

TEST(BacktestQueue, TobDoesNotFillUntilQueueAheadConsumed)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  // Order sits at best bid behind 5 units of existing queue.
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  EXPECT_EQ(exec.fills().size(), 0u);

  // Trade of 3 units consumes part of queue ahead, still no fill.
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(3.0), false);
  EXPECT_EQ(exec.fills().size(), 0u);

  // Trade of 3 more: 2 of them consume remaining queue-ahead, 1 fills our order.
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(3.0), false);
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].quantity.toDouble(), 1.0);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 100.0);
}

TEST(BacktestQueue, PartialFill)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  // Our order is alone at the level (queue-ahead = 0 via flat book assumption
  // but we set level qty = 0 at submit for easy setup).
  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 10.0));

  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(3.0), false);
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].quantity.toDouble(), 3.0);

  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(5.0), false);
  ASSERT_EQ(exec.fills().size(), 2u);
  EXPECT_DOUBLE_EQ(exec.fills()[1].quantity.toDouble(), 5.0);
}

TEST(BacktestQueue, CancelInFrontShrinksQueueAhead)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  // Our order behind 10 units.
  pushBook(exec, 1, 100.0, 10.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  EXPECT_EQ(exec.fills().size(), 0u);

  // Level shrinks to 1 without any trade: cancels in front removed 9 units.
  pushBook(exec, 1, 100.0, 1.0, 101.0, 5.0);
  EXPECT_EQ(exec.fills().size(), 0u);

  // A trade that exceeds remaining queue-ahead reaches us.
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(2.0), false);
  EXPECT_EQ(exec.fills().size(), 1u);
}

TEST(BacktestQueue, NoneModelFillsARestingLimitAtItsPostedPriceAsMaker)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  // Default queue model is NONE: no queue position is modelled, but the
  // economics of a resting order still have to hold.
  pushBook(exec, 1, 100.0, 10.0, 105.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 102.0, 1.0));
  EXPECT_EQ(exec.fills().size(), 0u);

  // Best ask drops to 101, crossing our resting bid. Someone sold through 102
  // to get there, so the fill happens at 102 and it is a maker fill.
  pushBook(exec, 1, 100.0, 10.0, 101.0, 5.0);
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 102.0);
  EXPECT_TRUE(exec.fills()[0].isMaker);
}

TEST(BacktestQueue, QueuePositionUpdatedEmittedOnTradeAhead)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setQueuePositionMinChangeFraction(0.05);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // Order behind 10 units of resting bid quantity.
  pushBook(exec, 1, 100.0, 10.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));

  // Trade of 5 consumes half the queue ahead, no fill but queue moved.
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(5.0), false);

  size_t queueEvents = 0;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::QUEUE_POSITION_UPDATED)
    {
      ++queueEvents;
      EXPECT_LT(ev.queueAhead.toDouble(), 10.0);
      EXPECT_GE(ev.queueAhead.toDouble(), 0.0);
      EXPECT_EQ(ev.order.id, 1u);
    }
  }
  EXPECT_GE(queueEvents, 1u);
}

TEST(BacktestQueue, QueuePositionFractionThresholdSuppressesSmallMoves)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setQueuePositionMinChangeFraction(0.50);  // require 50% shift

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 10.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));

  // Trade of 1 = 10% shift, below 50% threshold → suppressed.
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);

  for (const auto& ev : events)
  {
    EXPECT_NE(ev.status, OrderEventStatus::QUEUE_POSITION_UPDATED);
  }
}

TEST(BacktestQueue, FillCarriesQueueAheadAndTotal)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // Order alone at the level (queueAhead = 0 at arrival).
  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 3.0));

  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(2.0), false);

  bool sawFill = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::PARTIALLY_FILLED ||
        ev.status == OrderEventStatus::FILLED)
    {
      sawFill = true;
      EXPECT_GE(ev.queueAhead.raw(), 0);
    }
  }
  EXPECT_TRUE(sawFill);
}

// A print is one trade between one buyer and one seller. It consumes resting
// liquidity on exactly one side of the book -- the side the aggressor hit.
TEST(BacktestQueue, ATradeOnlyConsumesTheSideTheAggressorHit)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  // Two-sided quote, both legs alone at their level.
  // Both legs rest at 100: neither crosses a 99 / 101 book.
  pushBook(exec, 1, 99.0, 0.0, 101.0, 0.0);
  exec.submitOrder(limitBuy(11, 1, 100.0, 3.0));
  exec.submitOrder(limitSell(22, 1, 100.0, 3.0));

  // A buyer lifts 3 lots. Only the resting ask can be on the other side.
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(3.0), true);

  double totalFilled = 0.0;
  for (const auto& f : exec.fills())
  {
    totalFilled += f.quantity.toDouble();
    EXPECT_EQ(f.side, Side::SELL);
    EXPECT_EQ(f.orderId, 22u);
  }
  EXPECT_DOUBLE_EQ(totalFilled, 3.0);
}

TEST(BacktestQueue, ASellAggressorConsumesTheRestingBid)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  // Both legs rest at 100: neither crosses a 99 / 101 book.
  pushBook(exec, 1, 99.0, 0.0, 101.0, 0.0);
  exec.submitOrder(limitBuy(11, 1, 100.0, 3.0));
  exec.submitOrder(limitSell(22, 1, 100.0, 3.0));

  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(3.0), false);

  double totalFilled = 0.0;
  for (const auto& f : exec.fills())
  {
    totalFilled += f.quantity.toDouble();
    EXPECT_EQ(f.side, Side::BUY);
    EXPECT_EQ(f.orderId, 11u);
  }
  EXPECT_DOUBLE_EQ(totalFilled, 3.0);
}

TEST(BacktestQueue, TwoSidedQuoteCannotSelfTradeOnASinglePrint)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  // Both legs rest at 100: neither crosses a 99 / 101 book.
  pushBook(exec, 1, 99.0, 0.0, 101.0, 0.0);
  exec.submitOrder(limitBuy(11, 1, 100.0, 3.0));
  exec.submitOrder(limitSell(22, 1, 100.0, 3.0));

  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(3.0), true);

  double net = 0.0;
  double gross = 0.0;
  for (const auto& f : exec.fills())
  {
    const double signed_ = (f.side == Side::BUY) ? f.quantity.toDouble()
                                                 : -f.quantity.toDouble();
    net += signed_;
    gross += f.quantity.toDouble();
  }
  EXPECT_DOUBLE_EQ(gross, 3.0);
  EXPECT_NE(net, 0.0);
}

TEST(BacktestQueue, CancelOrderRemovesFromQueue)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(42, 1, 100.0, 1.0));
  exec.cancelOrder(42);

  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(10.0), false);
  EXPECT_EQ(exec.fills().size(), 0u);
}
