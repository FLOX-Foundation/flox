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

#include <vector>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

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

Order makeLimit(OrderId id, Side side, double price, double qty)
{
  Order o;
  o.id = id;
  o.symbol = BTC;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

struct Cap
{
  std::vector<OrderEvent> events;
  void on(const OrderEvent& e) { events.push_back(e); }
  size_t count(OrderEventStatus s) const
  {
    size_t n = 0;
    for (const auto& e : events)
    {
      n += (e.status == s);
    }
    return n;
  }
  std::string lastReject() const
  {
    for (auto it = events.rbegin(); it != events.rend(); ++it)
    {
      if (it->status == OrderEventStatus::REJECTED)
      {
        return it->rejectReason;
      }
    }
    return "";
  }
};
}  // namespace

TEST(STPModes, NoneAllowsCrossingResting)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });

  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);  // wide spread

  exec.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0));
  exec.submitOrder(makeLimit(2, Side::SELL, 50000.0, 1.0));  // crosses our BUY

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 0u);
}

TEST(STPModes, CancelNewestRejectsIncoming)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelNewest);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });

  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0));
  cap.events.clear();
  exec.submitOrder(makeLimit(2, Side::SELL, 50000.0, 1.0));

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 1u);
  EXPECT_EQ(cap.lastReject(), "stp_cancel_newest");
}

TEST(STPModes, CancelOldestRemovesRestingAndAcceptsNew)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelOldest);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });

  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);
  exec.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0));
  cap.events.clear();

  exec.submitOrder(makeLimit(2, Side::SELL, 50000.0, 1.0));

  EXPECT_EQ(cap.count(OrderEventStatus::CANCELED), 1u);
  // Accepted order proceeds (SUBMITTED + ACCEPTED emitted by normal path).
  EXPECT_GE(cap.count(OrderEventStatus::ACCEPTED), 1u);
}

TEST(STPModes, CancelBothCancelsExistingAndRejectsNew)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelBoth);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });

  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);
  exec.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0));
  cap.events.clear();

  exec.submitOrder(makeLimit(2, Side::SELL, 50000.0, 1.0));

  EXPECT_EQ(cap.count(OrderEventStatus::CANCELED), 1u);
  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 1u);
  EXPECT_EQ(cap.lastReject(), "stp_cancel_both");
}

TEST(STPModes, DecrementShrinksLargerSideWhenIncomingIsSmaller)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::Decrement);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });

  pushBook(exec, BTC, 49000.0, 5.0, 51000.0, 5.0);
  exec.submitOrder(makeLimit(1, Side::BUY, 50500.0, 5.0));  // resting larger
  cap.events.clear();

  // Incoming SELL is smaller. Cancel new, shrink existing.
  exec.submitOrder(makeLimit(2, Side::SELL, 50000.0, 2.0));

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 1u);
  EXPECT_EQ(cap.lastReject(), "stp_decrement_newest");
}

TEST(STPModes, DecrementCancelsExistingWhenIncomingIsLarger)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::Decrement);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });

  pushBook(exec, BTC, 49000.0, 5.0, 51000.0, 5.0);
  exec.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0));  // resting smaller
  cap.events.clear();

  // Incoming SELL is larger. Cancel existing, accept new with truncation.
  exec.submitOrder(makeLimit(2, Side::SELL, 50000.0, 3.0));

  EXPECT_EQ(cap.count(OrderEventStatus::CANCELED), 1u);
  // Accepted side proceeds (SUBMITTED + ACCEPTED).
  EXPECT_GE(cap.count(OrderEventStatus::ACCEPTED), 1u);
}

TEST(STPModes, NonCrossingOppositeSideDoesNotTrigger)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelNewest);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });

  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 49500.0, 1.0));
  cap.events.clear();
  // SELL @ 50500 does NOT cross the BUY @ 49500.
  exec.submitOrder(makeLimit(2, Side::SELL, 50500.0, 1.0));

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 0u);
}

TEST(STPModes, SameSideNeverTriggers)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelNewest);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });

  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0));
  cap.events.clear();
  exec.submitOrder(makeLimit(2, Side::BUY, 50500.0, 1.0));  // same side, same price

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 0u);
}
