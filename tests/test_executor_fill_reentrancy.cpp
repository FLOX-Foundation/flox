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

#include <cstddef>
#include <vector>

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

Order limitOrder(OrderId id, SymbolId sym, Side side, double price, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}
}  // namespace

// A fill callback is a strategy callback: it submits, cancels and replaces
// orders. Every one of those reshapes the executor's pending-order storage
// while the fill that triggered the callback is still being processed. The
// linked leg of an OCO pair has to be cancelled no matter how many orders are
// live at the time.
//
// The follow-up count sweeps across the executor's initial pending-order
// capacity, which is where the storage relocates.
TEST(ExecutorFillReentrancy, OcoSiblingIsCancelledRegardlessOfHowManyOrdersAreLive)
{
  const size_t followUpCounts[] = {0u, 40u, 63u,
                                   SimulatedExecutor::kDefaultOrderCapacity,
                                   100u, 400u};

  for (size_t followUps : followUpCounts)
  {
    SimulatedClock clock;
    SimulatedExecutor exec(clock);
    exec.setQueueModel(QueueModel::TOB, 1);

    size_t legBFills = 0;
    size_t legBCancels = 0;
    OrderId nextFollowUp = 1000;
    bool spawned = false;

    exec.setOrderEventCallback(
        [&](const OrderEvent& ev)
        {
          if (ev.order.id == 2)
          {
            if (ev.status == OrderEventStatus::CANCELED)
            {
              ++legBCancels;
            }
            if (ev.status == OrderEventStatus::FILLED ||
                ev.status == OrderEventStatus::PARTIALLY_FILLED)
            {
              ++legBFills;
            }
          }
          const bool legAFilled =
              (ev.order.id == 1 && ev.status == OrderEventStatus::FILLED);
          if (!legAFilled || spawned)
          {
            return;
          }
          spawned = true;
          for (size_t i = 0; i < followUps; ++i)
          {
            exec.submitOrder(limitOrder(nextFollowUp++, 1, Side::BUY, 50.0, 1.0));
          }
        });

    pushBook(exec, 1, 99.0, 0.0, 101.0, 0.0);

    OCOParams oco;
    oco.order1 = limitOrder(1, 1, Side::BUY, 99.0, 5.0);
    oco.order2 = limitOrder(2, 1, Side::SELL, 101.0, 5.0);
    exec.submitOCO(oco);

    // An aggressive sell walks leg A out of the queue.
    exec.onTrade(1, Price::fromDouble(99.0), Quantity::fromDouble(15.0), false);

    EXPECT_EQ(legBCancels, 1u) << "followUps = " << followUps;
    EXPECT_EQ(legBFills, 0u) << "followUps = " << followUps;

    // Leg B must be gone, so crossing its price fills nothing.
    const size_t fillsBefore = exec.fills().size();
    pushBook(exec, 1, 102.0, 5.0, 103.0, 5.0);
    for (size_t i = fillsBefore; i < exec.fills().size(); ++i)
    {
      EXPECT_NE(exec.fills()[i].orderId, 2u) << "followUps = " << followUps;
    }
  }
}

// Cancelling from inside a fill callback rewrites the same storage without
// needing it to grow, so it has no threshold at all.
TEST(ExecutorFillReentrancy, CancelFromInsideAFillCallbackLeavesTheRestIntact)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderId> canceled;
  bool acted = false;

  exec.setOrderEventCallback(
      [&](const OrderEvent& ev)
      {
        if (ev.status == OrderEventStatus::CANCELED)
        {
          canceled.push_back(ev.order.id);
        }
        if (ev.order.id != 1 || ev.status != OrderEventStatus::FILLED || acted)
        {
          return;
        }
        acted = true;
        exec.cancelOrder(10);
        exec.cancelOrder(11);
      });

  pushBook(exec, 1, 99.0, 0.0, 101.0, 0.0);
  exec.submitOrder(limitOrder(1, 1, Side::BUY, 99.0, 5.0));
  exec.submitOrder(limitOrder(10, 1, Side::BUY, 98.0, 5.0));
  exec.submitOrder(limitOrder(11, 1, Side::BUY, 97.0, 5.0));
  exec.submitOrder(limitOrder(12, 1, Side::BUY, 96.0, 5.0));

  exec.onTrade(1, Price::fromDouble(99.0), Quantity::fromDouble(5.0), false);

  ASSERT_EQ(canceled.size(), 2u);
  EXPECT_EQ(canceled[0], 10u);
  EXPECT_EQ(canceled[1], 11u);

  // Order 12 is untouched and still fills when the book reaches it.
  exec.onTrade(1, Price::fromDouble(96.0), Quantity::fromDouble(5.0), false);
  bool filled12 = false;
  for (const auto& f : exec.fills())
  {
    if (f.orderId == 12)
    {
      filled12 = true;
    }
  }
  EXPECT_TRUE(filled12);
}

// The same applies to the book-driven sweep, which is the path a run with no
// queue model takes. A cancel from inside one order's fill callback shortens
// the pending-order list the sweep is walking.
TEST(ExecutorFillReentrancy, BookDrivenSweepSurvivesACancelFromAFillCallback)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  std::vector<OrderId> canceled;
  bool acted = false;

  exec.setOrderEventCallback(
      [&](const OrderEvent& ev)
      {
        if (ev.status == OrderEventStatus::CANCELED)
        {
          canceled.push_back(ev.order.id);
        }
        if (ev.order.id != 1 || ev.status != OrderEventStatus::FILLED || acted)
        {
          return;
        }
        acted = true;
        exec.cancelOrder(2);
        exec.cancelOrder(3);
      });

  // The order that fills is submitted last, so the sweep is looking at the
  // final slot when the callback shortens the list under it.
  pushBook(exec, 1, 99.0, 10.0, 101.0, 10.0);
  exec.submitOrder(limitOrder(2, 1, Side::BUY, 95.0, 1.0));
  exec.submitOrder(limitOrder(3, 1, Side::BUY, 94.0, 1.0));
  exec.submitOrder(limitOrder(4, 1, Side::BUY, 93.0, 1.0));
  exec.submitOrder(limitOrder(1, 1, Side::BUY, 100.0, 1.0));
  ASSERT_EQ(exec.fills().size(), 0u);

  // The ask comes down through order 1 only.
  pushBook(exec, 1, 99.5, 10.0, 99.8, 10.0);

  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_EQ(exec.fills()[0].orderId, 1u);
  ASSERT_EQ(canceled.size(), 2u);
  EXPECT_EQ(canceled[0], 2u);
  EXPECT_EQ(canceled[1], 3u);

  // Order 4 is untouched and still fills when the book reaches it.
  pushBook(exec, 1, 92.0, 10.0, 92.5, 10.0);
  bool filled4 = false;
  for (const auto& f : exec.fills())
  {
    if (f.orderId == 4)
    {
      filled4 = true;
    }
  }
  EXPECT_TRUE(filled4);
}

// An OCO pair resolved off the book-driven sweep: the fill of one leg cancels
// the other from inside the sweep that is still walking the list.
TEST(ExecutorFillReentrancy, BookDrivenSweepResolvesAnOcoPair)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  size_t legBCancels = 0;
  size_t legBFills = 0;
  exec.setOrderEventCallback(
      [&](const OrderEvent& ev)
      {
        if (ev.order.id != 1)
        {
          return;
        }
        if (ev.status == OrderEventStatus::CANCELED)
        {
          ++legBCancels;
        }
        if (ev.status == OrderEventStatus::FILLED ||
            ev.status == OrderEventStatus::PARTIALLY_FILLED)
        {
          ++legBFills;
        }
      });

  pushBook(exec, 1, 99.0, 10.0, 101.0, 10.0);

  // The leg that fills is the second one submitted, so it occupies the slot
  // the sweep is looking at when its own sibling's cancel shortens the list.
  OCOParams oco;
  oco.order1 = limitOrder(1, 1, Side::SELL, 100.5, 1.0);
  oco.order2 = limitOrder(2, 1, Side::BUY, 100.0, 1.0);
  exec.submitOCO(oco);

  // Sweeps down through the buy leg only.
  pushBook(exec, 1, 99.5, 10.0, 99.8, 10.0);

  EXPECT_EQ(legBCancels, 1u);
  EXPECT_EQ(legBFills, 0u);
}

// Wiping every order from inside a fill callback must not leave the executor
// reading storage it has already dropped.
TEST(ExecutorFillReentrancy, CancelAllFromInsideAFillCallbackIsSafe)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  bool acted = false;
  exec.setOrderEventCallback(
      [&](const OrderEvent& ev)
      {
        if (ev.order.id != 1 || ev.status != OrderEventStatus::FILLED || acted)
        {
          return;
        }
        acted = true;
        exec.cancelAllOrders(1);
      });

  pushBook(exec, 1, 99.0, 0.0, 101.0, 0.0);
  exec.submitOrder(limitOrder(1, 1, Side::BUY, 99.0, 5.0));
  for (OrderId id = 20; id < 40; ++id)
  {
    exec.submitOrder(limitOrder(id, 1, Side::BUY, 98.0, 1.0));
  }

  exec.onTrade(1, Price::fromDouble(99.0), Quantity::fromDouble(5.0), false);
  EXPECT_TRUE(acted);

  // Nothing is left, so a sweep through the whole book fills nothing more.
  const size_t before = exec.fills().size();
  exec.onTrade(1, Price::fromDouble(98.0), Quantity::fromDouble(100.0), false);
  EXPECT_EQ(exec.fills().size(), before);
}
