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

// === Multi-account STP ===

TEST(STPModes, DifferentAccountsDoNotTrigger)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelNewest);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });
  pushBook(exec, BTC, 49000.0, 5.0, 51000.0, 5.0);

  // Two separate accounts crossing at 50500. With no group mapping,
  // they're considered independent — no STP.
  Order a = makeLimit(1, Side::BUY, 50500.0, 1.0);
  a.accountId = 42;
  exec.submitOrder(a);
  cap.events.clear();
  Order b = makeLimit(2, Side::SELL, 50500.0, 1.0);
  b.accountId = 43;
  exec.submitOrder(b);

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 0u);
}

TEST(STPModes, SameAccountStillTriggers)
{
  // Same accountId — STP still fires (parity with the existing default STP behaviour).
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelNewest);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });
  pushBook(exec, BTC, 49000.0, 5.0, 51000.0, 5.0);

  Order a = makeLimit(1, Side::BUY, 50500.0, 1.0);
  a.accountId = 42;
  exec.submitOrder(a);
  cap.events.clear();
  Order b = makeLimit(2, Side::SELL, 50500.0, 1.0);
  b.accountId = 42;
  exec.submitOrder(b);

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 1u);
  EXPECT_EQ(cap.lastReject(), "stp_cancel_newest");
}

TEST(STPModes, SameGroupTriggersAcrossAccounts)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelNewest);
  exec.setSTPGroupMembership(42, 100);
  exec.setSTPGroupMembership(43, 100);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });
  pushBook(exec, BTC, 49000.0, 5.0, 51000.0, 5.0);

  Order a = makeLimit(1, Side::BUY, 50500.0, 1.0);
  a.accountId = 42;
  exec.submitOrder(a);
  cap.events.clear();
  Order b = makeLimit(2, Side::SELL, 50500.0, 1.0);
  b.accountId = 43;
  exec.submitOrder(b);

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 1u);
}

TEST(STPModes, MixedGroupAndUngroupedDoesNotTrigger)
{
  // One account is in a group, the other isn't. They don't share an
  // STP scope → no STP.
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelNewest);
  exec.setSTPGroupMembership(42, 100);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });
  pushBook(exec, BTC, 49000.0, 5.0, 51000.0, 5.0);

  Order a = makeLimit(1, Side::BUY, 50500.0, 1.0);
  a.accountId = 42;
  exec.submitOrder(a);
  cap.events.clear();
  Order b = makeLimit(2, Side::SELL, 50500.0, 1.0);
  b.accountId = 43;  // no group
  exec.submitOrder(b);

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 0u);
}

TEST(STPModes, DefaultAccountIdZeroPreservesLegacySemantics)
{
  // No accountId set anywhere → both orders default to 0 → STP fires.
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelNewest);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });
  pushBook(exec, BTC, 49000.0, 5.0, 51000.0, 5.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0));
  cap.events.clear();
  exec.submitOrder(makeLimit(2, Side::SELL, 50500.0, 1.0));

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 1u);
}

// Decrement shrinks the resting order. The queue tracker holds the size that
// order can still trade, so it has to shrink with it -- otherwise the order
// keeps trading the size it no longer has.
TEST(STPModes, DecrementShrinksWhatTheOrderCanStillTrade)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::Decrement);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });
  pushBook(exec, BTC, 100.0, 0.0, 101.0, 5.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 100.0, 10.0));
  // Opposite 4 lots decrements the resting order from 10 to 6.
  exec.submitOrder(makeLimit(2, Side::SELL, 100.0, 4.0));
  EXPECT_EQ(cap.lastReject(), "stp_decrement_newest");

  // A 30-lot print at our price can take at most the 6 lots that are left.
  exec.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(30.0), false);

  double filled = 0.0;
  for (const auto& f : exec.fills())
  {
    filled += f.quantity.toDouble();
  }
  EXPECT_DOUBLE_EQ(filled, 6.0);
}

// When the incoming order matches the resting remainder exactly, the resting
// order is left with nothing to trade. A venue pulls such an order; it must
// not keep standing in the queue for its original size.
TEST(STPModes, DecrementToZeroCancelsTheRestingOrder)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::Decrement);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });
  pushBook(exec, BTC, 100.0, 0.0, 101.0, 5.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 100.0, 10.0));
  exec.submitOrder(makeLimit(2, Side::SELL, 100.0, 10.0));

  EXPECT_EQ(cap.count(OrderEventStatus::CANCELED), 1u);

  exec.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(30.0), false);

  double filled = 0.0;
  for (const auto& f : exec.fills())
  {
    filled += f.quantity.toDouble();
  }
  EXPECT_DOUBLE_EQ(filled, 0.0);
}

// Self-trade prevention runs after the reduce-only check, the way the guide
// describes it. An order the engine is about to reject on its own terms must
// not get to touch anything already resting.
TEST(STPModes, ARejectedReduceOnlyOrderLeavesTheRestingOrderAlone)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::CancelOldest);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });
  pushBook(exec, BTC, 100.0, 0.0, 101.0, 5.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 100.0, 5.0));

  // Flat position, so this reduce-only order reduces nothing and is rejected.
  Order ro = makeLimit(2, Side::SELL, 100.0, 5.0);
  ro.flags.reduceOnly = true;
  exec.submitOrder(ro);

  EXPECT_EQ(cap.lastReject(), "reduce_only");
  EXPECT_EQ(cap.count(OrderEventStatus::CANCELED), 0u);

  // The resting order survived and still trades.
  exec.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(5.0), false);
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_EQ(exec.fills()[0].orderId, 1u);
}

TEST(STPModes, ARejectedReduceOnlyOrderDoesNotShrinkTheRestingOrder)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setSTPMode(STPMode::Decrement);
  Cap cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.on(e); });
  pushBook(exec, BTC, 100.0, 0.0, 101.0, 5.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 100.0, 5.0));

  Order ro = makeLimit(2, Side::SELL, 100.0, 3.0);
  ro.flags.reduceOnly = true;
  exec.submitOrder(ro);

  EXPECT_EQ(cap.lastReject(), "reduce_only");

  exec.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(10.0), false);
  double filled = 0.0;
  for (const auto& f : exec.fills())
  {
    filled += f.quantity.toDouble();
  }
  EXPECT_DOUBLE_EQ(filled, 5.0);
}
