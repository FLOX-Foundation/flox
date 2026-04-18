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

TEST(BacktestQueue, NoneModelPreservesLegacyInstantFill)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  // Default queue model is NONE.
  pushBook(exec, 1, 100.0, 10.0, 105.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 102.0, 1.0));
  EXPECT_EQ(exec.fills().size(), 0u);

  // Best ask drops to 101 (crosses our limit). Without queue, fill immediately.
  pushBook(exec, 1, 100.0, 10.0, 101.0, 5.0);
  ASSERT_EQ(exec.fills().size(), 1u);
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
