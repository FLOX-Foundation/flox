/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// W33-T014, core finding 6.
//
// OrderState::isTerminal() lists FILLED / CANCELED / REJECTED / EXPIRED but
// not REPLACED, which is the status OrderTracker::onReplaced() writes on the
// superseded order. A replaced order therefore stays "active" for the rest of
// the process: isActive() reports true, activeOrderCount() counts it, and
// pruneTerminal() refuses to drop it, so _orders grows by one entry per
// replace on a quoting strategy that amends continuously.

#include "flox/engine/engine_config.h"
#include "flox/execution/order_tracker.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

Order makeOrder(OrderId id)
{
  Order order;
  order.id = id;
  order.symbol = 7;
  order.side = Side::BUY;
  order.price = Price::fromDouble(100.0);
  order.quantity = Quantity::fromDouble(1.0);
  return order;
}

}  // namespace

TEST(OrderTrackerReplacedTest, ReplacedOrderIsTerminal)
{
  OrderTracker tracker;

  ASSERT_TRUE(tracker.onSubmitted(makeOrder(1), "ex-1"));
  ASSERT_TRUE(tracker.onReplaced(1, makeOrder(2), "ex-2"));

  auto superseded = tracker.get(1);
  ASSERT_TRUE(superseded.has_value());
  EXPECT_EQ(superseded->status, OrderEventStatus::REPLACED);
  EXPECT_TRUE(superseded->isTerminal());

  EXPECT_FALSE(tracker.isActive(1));
  EXPECT_TRUE(tracker.isActive(2));
  EXPECT_EQ(tracker.activeOrderCount(), 1u);
  EXPECT_EQ(tracker.totalOrderCount(), 2u);
}

// A replaced order is terminal, so no further lifecycle event may move it.
TEST(OrderTrackerReplacedTest, ReplacedOrderRefusesFurtherTransitions)
{
  OrderTracker tracker;

  ASSERT_TRUE(tracker.onSubmitted(makeOrder(1), "ex-1"));
  ASSERT_TRUE(tracker.onReplaced(1, makeOrder(2), "ex-2"));

  EXPECT_FALSE(tracker.onFilled(1, Quantity::fromDouble(0.5)));
  EXPECT_FALSE(tracker.onCanceled(1));
  EXPECT_FALSE(tracker.onExpired(1));
  EXPECT_FALSE(tracker.onPendingCancel(1));
  EXPECT_FALSE(tracker.onRejected(1, "late reject"));
  EXPECT_EQ(tracker.getStatus(1), OrderEventStatus::REPLACED);

  // onFilled() has to ask isTerminal() rather than carry its own list of
  // terminal statuses: a private list is what let a replaced order keep
  // accumulating fills.
  auto superseded = tracker.get(1);
  ASSERT_TRUE(superseded.has_value());
  EXPECT_EQ(superseded->filled.raw(), 0);
  EXPECT_EQ(tracker.activeOrderCount(), 1u);
}

// The same private-list hazard on the other status onFilled() used to miss.
TEST(OrderTrackerReplacedTest, FilledOrderRefusesAnotherFill)
{
  OrderTracker tracker;

  ASSERT_TRUE(tracker.onSubmitted(makeOrder(1), "ex-1"));
  ASSERT_TRUE(tracker.onFilled(1, Quantity::fromDouble(1.0)));
  ASSERT_EQ(tracker.getStatus(1), OrderEventStatus::FILLED);

  EXPECT_FALSE(tracker.onFilled(1, Quantity::fromDouble(0.5)));

  auto state = tracker.get(1);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->status, OrderEventStatus::FILLED);
  EXPECT_EQ(state->filled.raw(), Quantity::fromDouble(1.0).raw());
}

// An amend on a tracker saturated with live orders succeeds by recycling the
// entry it supersedes. The superseded order has to be marked REPLACED before
// room is requested: ask first and the old order is still counted as live, so
// pruning cannot reclaim it and the replace is refused - which drops the
// venue's acknowledgement of an order that is already working.
TEST(OrderTrackerReplacedTest, AmendAtCapacityRecyclesTheSupersededOrder)
{
  OrderTracker tracker{4};

  for (OrderId id = 1; id <= 4; ++id)
  {
    ASSERT_TRUE(tracker.onSubmitted(makeOrder(id), "ex"));
  }
  ASSERT_EQ(tracker.totalOrderCount(), 4u);
  ASSERT_EQ(tracker.activeOrderCount(), 4u);

  EXPECT_TRUE(tracker.onReplaced(1, makeOrder(5), "ex-5"));

  EXPECT_EQ(tracker.totalOrderCount(), 4u);
  EXPECT_EQ(tracker.activeOrderCount(), 4u);
  EXPECT_TRUE(tracker.isActive(5));
  EXPECT_FALSE(tracker.isActive(1));
  for (OrderId id = 2; id <= 4; ++id)
  {
    EXPECT_TRUE(tracker.isActive(id)) << "live order " << id << " was lost to the amend";
  }
}

// isTerminal() must stay narrow: a live order is not swept away by a fix that
// simply returns true for everything.
TEST(OrderTrackerReplacedTest, LiveOrdersStayActiveAndSurvivePruning)
{
  OrderTracker tracker;

  ASSERT_TRUE(tracker.onSubmitted(makeOrder(1), "ex-1"));
  ASSERT_TRUE(tracker.onSubmitted(makeOrder(2), "ex-2"));
  ASSERT_TRUE(tracker.onFilled(2, Quantity::fromDouble(0.25)));
  ASSERT_TRUE(tracker.onPendingCancel(1));

  EXPECT_EQ(tracker.getStatus(2), OrderEventStatus::PARTIALLY_FILLED);
  EXPECT_EQ(tracker.activeOrderCount(), 2u);

  tracker.pruneTerminal();

  EXPECT_EQ(tracker.totalOrderCount(), 2u);
  EXPECT_TRUE(tracker.exists(1));
  EXPECT_TRUE(tracker.exists(2));
}

TEST(OrderTrackerReplacedTest, PruneTerminalCollapsesAReplaceChain)
{
  OrderTracker tracker;

  constexpr int kAmends = 64;

  OrderId current = 1;
  ASSERT_TRUE(tracker.onSubmitted(makeOrder(current), "ex-1"));
  for (int i = 0; i < kAmends; ++i)
  {
    const OrderId next = current + 1;
    ASSERT_TRUE(tracker.onReplaced(current, makeOrder(next), "ex"));
    current = next;
  }

  EXPECT_EQ(tracker.activeOrderCount(), 1u);
  EXPECT_EQ(tracker.totalOrderCount(), static_cast<size_t>(kAmends) + 1u);

  tracker.pruneTerminal();

  EXPECT_EQ(tracker.totalOrderCount(), 1u);
  EXPECT_EQ(tracker.activeOrderCount(), 1u);
  ASSERT_TRUE(tracker.exists(current));
  EXPECT_EQ(tracker.getStatus(current), OrderEventStatus::SUBMITTED);
  EXPECT_FALSE(tracker.exists(1));
}

// Nothing calls pruneTerminal() on the live path, so the tracker has to bound
// itself: a long amend chain must not push the map past the configured
// capacity, and the one live order must survive the trimming.
TEST(OrderTrackerReplacedTest, ReplaceChainStaysWithinDefaultCapacity)
{
  OrderTracker tracker;

  const size_t capacity = static_cast<size_t>(config::ORDER_TRACKER_CAPACITY);
  const size_t amends = capacity * 2;

  OrderId current = 1;
  ASSERT_TRUE(tracker.onSubmitted(makeOrder(current), "ex-1"));
  for (size_t i = 0; i < amends; ++i)
  {
    const OrderId next = current + 1;
    ASSERT_TRUE(tracker.onReplaced(current, makeOrder(next), "ex"));
    current = next;
    ASSERT_LE(tracker.totalOrderCount(), capacity) << "after " << (i + 1) << " amends";
  }

  EXPECT_EQ(tracker.activeOrderCount(), 1u);
  ASSERT_TRUE(tracker.exists(current));
  EXPECT_EQ(tracker.getStatus(current), OrderEventStatus::SUBMITTED);
}

// Same bound on the plain submit/cancel cycle: terminal entries are recycled,
// never accumulated.
TEST(OrderTrackerReplacedTest, SubmitCancelCyclesStayWithinDefaultCapacity)
{
  OrderTracker tracker;

  const size_t capacity = static_cast<size_t>(config::ORDER_TRACKER_CAPACITY);
  const size_t cycles = capacity * 2;

  for (size_t i = 0; i < cycles; ++i)
  {
    const OrderId id = static_cast<OrderId>(i) + 1;
    ASSERT_TRUE(tracker.onSubmitted(makeOrder(id), "ex"));
    ASSERT_TRUE(tracker.onCanceled(id));
    ASSERT_LE(tracker.totalOrderCount(), capacity) << "after " << (i + 1) << " cycles";
  }

  EXPECT_EQ(tracker.activeOrderCount(), 0u);
}
