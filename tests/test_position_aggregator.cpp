/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include "flox/execution/multi_execution_listener.h"
#include "flox/position/multi_mode_position_tracker.h"
#include "flox/position/position_group.h"
#include "flox/position/position_reconciler.h"

using namespace flox;

static Order makeOrder(OrderId id, SymbolId sym, Side side, double price, double qty,
                       bool reduceOnly = false, bool closePos = false, uint16_t tag = 0)
{
  Order o{};
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  o.flags.reduceOnly = reduceOnly ? 1 : 0;
  o.flags.closePosition = closePos ? 1 : 0;
  o.orderTag = tag;
  return o;
}

class NetModeTest : public ::testing::Test
{
 protected:
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::FIFO};
};

TEST_F(NetModeTest, BuyOpensLongPosition)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 50000.0, 1.0));
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 1.0);
}

TEST_F(NetModeTest, SellOpensShortPosition)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::SELL, 50000.0, 1.0));
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), -1.0);
}

TEST_F(NetModeTest, BuyThenSellCloses)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 50000.0, 1.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 51000.0, 1.0));
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 0.0);
  EXPECT_GT(tracker.getRealizedPnl(100).toDouble(), 0.0);
}

TEST_F(NetModeTest, PartialFillUpdatesPosition)
{
  auto order = makeOrder(1, 100, Side::BUY, 50000.0, 2.0);
  tracker.onOrderPartiallyFilled(order, Quantity::fromDouble(0.5));
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 0.5);
}

TEST_F(NetModeTest, RealizedPnlOnClose)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 10.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 110.0, 10.0));
  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 100.0, 0.01);
}

TEST_F(NetModeTest, FlipFromLongToShort)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 110.0, 8.0));
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), -3.0);
  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 50.0, 0.01);
}

TEST_F(NetModeTest, MultipleSymbols)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  tracker.onOrderFilled(makeOrder(2, 200, Side::SELL, 50.0, 3.0));
  tracker.onOrderFilled(makeOrder(3, 300, Side::BUY, 1000.0, 1.0));

  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(tracker.getPosition(200).toDouble(), -3.0);
  EXPECT_DOUBLE_EQ(tracker.getPosition(300).toDouble(), 1.0);
}

TEST_F(NetModeTest, MultipleSymbolsPnl)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 10.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 110.0, 10.0));
  tracker.onOrderFilled(makeOrder(3, 200, Side::SELL, 50.0, 4.0));
  tracker.onOrderFilled(makeOrder(4, 200, Side::BUY, 40.0, 4.0));

  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 100.0, 0.01);
  EXPECT_NEAR(tracker.getRealizedPnl(200).toDouble(), 40.0, 0.01);
  EXPECT_NEAR(tracker.getTotalRealizedPnl().toDouble(), 140.0, 0.01);
}

TEST_F(NetModeTest, ZeroFillDoesNothing)
{
  auto order = makeOrder(1, 100, Side::BUY, 100.0, 0.0);
  tracker.onOrderPartiallyFilled(order, Quantity::fromDouble(0.0));
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 0.0);
}

TEST(NetModeLIFO, ClosesNewestFirst)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::LIFO};

  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 1.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 200.0, 1.0));
  tracker.onOrderFilled(makeOrder(3, 100, Side::SELL, 250.0, 1.0));

  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 50.0, 0.01);
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 1.0);
}

TEST(NetModeFIFO, ClosesOldestFirst)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::FIFO};

  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 1.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 200.0, 1.0));
  tracker.onOrderFilled(makeOrder(3, 100, Side::SELL, 250.0, 1.0));

  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 150.0, 0.01);
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 1.0);
}

TEST(NetModeAVERAGE, UsesAveragePrice)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::AVERAGE};

  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 1.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 200.0, 1.0));
  tracker.onOrderFilled(makeOrder(3, 100, Side::SELL, 250.0, 1.0));

  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 100.0, 0.01);
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 1.0);
}

class PerSideModeTest : public ::testing::Test
{
 protected:
  MultiModePositionTracker tracker{1, PositionAggregationMode::PER_SIDE, CostBasisMethod::FIFO};
};

TEST_F(PerSideModeTest, BuyOpensLong)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 50000.0, 1.0));
  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 1.0);
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 0.0);
}

TEST_F(PerSideModeTest, SellOpensShort)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::SELL, 50000.0, 1.0));
  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 1.0);
}

TEST_F(PerSideModeTest, SimultaneousLongAndShort)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 50000.0, 2.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 51000.0, 1.0));

  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 2.0);
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 1.0);
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 1.0);
}

TEST_F(PerSideModeTest, AvgEntryPricesAreSeparate)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 1.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 200.0, 1.0));
  tracker.onOrderFilled(makeOrder(3, 100, Side::SELL, 300.0, 1.0));

  EXPECT_NEAR(tracker.getLongAvgEntry(100).toDouble(), 150.0, 0.01);
  EXPECT_NEAR(tracker.getShortAvgEntry(100).toDouble(), 300.0, 0.01);
}

TEST_F(PerSideModeTest, SellToCloseLong)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 120.0, 3.0, true));

  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 2.0);
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 0.0);
  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 60.0, 0.01);
}

TEST_F(PerSideModeTest, BuyToCloseShort)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::SELL, 200.0, 4.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 180.0, 2.0, false, true));

  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 2.0);
  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 0.0);
  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 40.0, 0.01);
}

TEST_F(PerSideModeTest, CloseMoreThanAvailable)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 3.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 120.0, 5.0, true));

  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 0.0);
  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 60.0, 0.01);
}

TEST_F(PerSideModeTest, CloseWithNoOppositePositionOpensInstead)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::SELL, 100.0, 2.0));
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 2.0);
  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 0.0);
}

TEST_F(PerSideModeTest, MultipleSymbolsPerSide)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 1.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 200.0, 2.0));
  tracker.onOrderFilled(makeOrder(3, 200, Side::BUY, 50.0, 5.0));
  tracker.onOrderFilled(makeOrder(4, 200, Side::SELL, 60.0, 3.0));

  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 1.0);
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 2.0);
  EXPECT_DOUBLE_EQ(tracker.getLongPosition(200).toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(200).toDouble(), 3.0);
}

TEST_F(PerSideModeTest, PartialFillPerSide)
{
  auto order = makeOrder(1, 100, Side::BUY, 100.0, 10.0);
  tracker.onOrderPartiallyFilled(order, Quantity::fromDouble(3.0));
  tracker.onOrderPartiallyFilled(order, Quantity::fromDouble(4.0));

  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 7.0);
}

TEST_F(PerSideModeTest, OpenLongExplicit)
{
  tracker.openLong(100, Price::fromDouble(100.0), Quantity::fromDouble(5.0));
  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 0.0);
}

TEST_F(PerSideModeTest, CloseLongExplicit)
{
  tracker.openLong(100, Price::fromDouble(100.0), Quantity::fromDouble(5.0));
  tracker.closeLong(100, Price::fromDouble(120.0), Quantity::fromDouble(3.0));

  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 2.0);
  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 60.0, 0.01);
}

TEST_F(PerSideModeTest, OpenShortExplicit)
{
  tracker.openShort(100, Price::fromDouble(200.0), Quantity::fromDouble(3.0));
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 3.0);
  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 0.0);
}

TEST_F(PerSideModeTest, CloseShortExplicit)
{
  tracker.openShort(100, Price::fromDouble(200.0), Quantity::fromDouble(4.0));
  tracker.closeShort(100, Price::fromDouble(180.0), Quantity::fromDouble(2.0));

  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 2.0);
  EXPECT_NEAR(tracker.getRealizedPnl(100).toDouble(), 40.0, 0.01);
}

TEST_F(PerSideModeTest, ExplicitFullRoundTrip)
{
  tracker.openLong(100, Price::fromDouble(100.0), Quantity::fromDouble(10.0));
  tracker.openShort(100, Price::fromDouble(200.0), Quantity::fromDouble(5.0));

  auto snap = tracker.snapshot(100);
  EXPECT_DOUBLE_EQ(snap.longQty.toDouble(), 10.0);
  EXPECT_DOUBLE_EQ(snap.shortQty.toDouble(), 5.0);

  tracker.closeLong(100, Price::fromDouble(110.0), Quantity::fromDouble(10.0));
  tracker.closeShort(100, Price::fromDouble(190.0), Quantity::fromDouble(5.0));

  snap = tracker.snapshot(100);
  EXPECT_DOUBLE_EQ(snap.longQty.toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(snap.shortQty.toDouble(), 0.0);
  // long PnL = (110-100)*10 = 100, short PnL = (200-190)*5 = 50
  EXPECT_NEAR(snap.realizedPnl.toDouble(), 150.0, 0.01);
}

TEST_F(PerSideModeTest, ExplicitWithTag)
{
  MultiModePositionTracker grouped{1, PositionAggregationMode::GROUPED};
  grouped.openLong(100, Price::fromDouble(100.0), Quantity::fromDouble(5.0), 42);
  grouped.closeLong(100, Price::fromDouble(120.0), Quantity::fromDouble(5.0), 42);

  EXPECT_EQ(grouped.groups().openPositionCount(100), 0);
  EXPECT_NEAR(grouped.getRealizedPnl(100).toDouble(), 100.0, 0.01);
}

TEST_F(PerSideModeTest, ReduceOnlyWithNoPositionIsNoop)
{
  // reduceOnly SELL with no long position -- should NOT open short
  tracker.onOrderFilled(makeOrder(1, 100, Side::SELL, 100.0, 2.0, true));
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 0.0);
}

class GroupedModeTest : public ::testing::Test
{
 protected:
  MultiModePositionTracker tracker{1, PositionAggregationMode::GROUPED, CostBasisMethod::FIFO};
};

TEST_F(GroupedModeTest, EachOrderCreatesPosition)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 50000.0, 1.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 51000.0, 0.5));

  EXPECT_EQ(tracker.groups().openPositionCount(100), 2);
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 1.5);
}

TEST_F(GroupedModeTest, ManualGrouping)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 1.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 200.0, 1.0));

  auto& groups = tracker.groups();

  auto* p1 = groups.getPositionByOrder(1);
  auto* p2 = groups.getPositionByOrder(2);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);

  GroupId gid = groups.createGroup();
  groups.assignToGroup(p1->positionId, gid);
  groups.assignToGroup(p2->positionId, gid);

  EXPECT_DOUBLE_EQ(groups.groupNetPosition(gid).toDouble(), 2.0);
}

TEST_F(GroupedModeTest, SubGroups)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 1.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 200.0, 1.0));
  tracker.onOrderFilled(makeOrder(3, 100, Side::SELL, 150.0, 2.0));

  auto& groups = tracker.groups();

  GroupId parent = groups.createGroup();
  GroupId child = groups.createGroup(parent);

  groups.assignToGroup(groups.getPositionByOrder(1)->positionId, parent);
  groups.assignToGroup(groups.getPositionByOrder(3)->positionId, child);

  EXPECT_DOUBLE_EQ(groups.groupNetPosition(parent).toDouble(), -1.0);
}

TEST_F(GroupedModeTest, AutoGroupByOrderTag)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 1.0, false, false, 42));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 200.0, 1.0, false, false, 42));
  tracker.onOrderFilled(makeOrder(3, 100, Side::SELL, 150.0, 0.5, false, false, 99));

  const auto& groups = tracker.groups();

  auto* p1 = groups.getPositionByOrder(1);
  auto* p2 = groups.getPositionByOrder(2);
  auto* p3 = groups.getPositionByOrder(3);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  ASSERT_NE(p3, nullptr);

  EXPECT_EQ(p1->groupId, p2->groupId);
  EXPECT_NE(p1->groupId, 0u);
  EXPECT_NE(p3->groupId, p1->groupId);
  EXPECT_NE(p3->groupId, 0u);
}

TEST_F(GroupedModeTest, CloseByTagWithReduceOnly)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0, false, false, 10));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 120.0, 3.0, true, false, 10));

  const auto& groups = tracker.groups();
  auto* p1 = groups.getPositionByOrder(1);
  ASSERT_NE(p1, nullptr);
  EXPECT_DOUBLE_EQ(p1->quantity.toDouble(), 2.0);
  EXPECT_NEAR(p1->realizedPnl.toDouble(), 60.0, 0.01);
}

TEST_F(GroupedModeTest, CloseFullPositionByTag)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0, false, false, 10));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 130.0, 5.0, true, false, 10));

  const auto& groups = tracker.groups();
  auto* p1 = groups.getPositionByOrder(1);
  ASSERT_NE(p1, nullptr);
  EXPECT_TRUE(p1->closed);
  EXPECT_NEAR(p1->realizedPnl.toDouble(), 150.0, 0.01);
  EXPECT_EQ(groups.openPositionCount(100), 0);
}

TEST_F(GroupedModeTest, MultiplePositionsInGroup)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 3.0, false, false, 10));
  tracker.onOrderFilled(makeOrder(2, 100, Side::BUY, 200.0, 2.0, false, false, 10));
  tracker.onOrderFilled(makeOrder(3, 100, Side::SELL, 150.0, 4.0, true, false, 10));

  const auto& groups = tracker.groups();
  auto* p1 = groups.getPositionByOrder(1);
  auto* p2 = groups.getPositionByOrder(2);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);

  EXPECT_TRUE(p1->closed);
  EXPECT_NEAR(p1->realizedPnl.toDouble(), 150.0, 0.01);

  EXPECT_FALSE(p2->closed);
  EXPECT_DOUBLE_EQ(p2->quantity.toDouble(), 1.0);
  EXPECT_NEAR(p2->realizedPnl.toDouble(), -50.0, 0.01);
}

TEST_F(GroupedModeTest, PartialFillSameOrder)
{
  auto order = makeOrder(1, 100, Side::BUY, 100.0, 10.0, false, false, 5);
  tracker.onOrderPartiallyFilled(order, Quantity::fromDouble(3.0));
  tracker.onOrderPartiallyFilled(order, Quantity::fromDouble(4.0));

  const auto& groups = tracker.groups();
  auto* p = groups.getPositionByOrder(1);
  ASSERT_NE(p, nullptr);
  EXPECT_DOUBLE_EQ(p->quantity.toDouble(), 7.0);
}

// Regression: reduceOnly with no tag should not create a position
TEST_F(GroupedModeTest, ReduceOnlyWithNoTagDoesNotCreatePosition)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  size_t before = tracker.groups().openPositionCount();

  // Close order with reduceOnly but no tag -- should not create new position
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 120.0, 3.0, true));

  EXPECT_EQ(tracker.groups().openPositionCount(), before);
}

// Regression: closePosition flag with no tag should not create position
TEST_F(GroupedModeTest, ClosePositionFlagNoTagDoesNotCreate)
{
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  size_t before = tracker.groups().openPositionCount();

  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 120.0, 3.0, false, true));

  EXPECT_EQ(tracker.groups().openPositionCount(), before);
}

TEST(PositionGroupTrackerTest, OpenAndClosePosition)
{
  PositionGroupTracker gt;

  PositionId pid = gt.openPosition(42, 100, Side::BUY, Price::fromDouble(100.0),
                                   Quantity::fromDouble(5.0));
  EXPECT_EQ(gt.openPositionCount(), 1);
  EXPECT_DOUBLE_EQ(gt.netPosition(100).toDouble(), 5.0);

  gt.closePosition(pid, Price::fromDouble(110.0));
  EXPECT_EQ(gt.openPositionCount(), 0);
  EXPECT_NEAR(gt.totalRealizedPnl().toDouble(), 50.0, 0.01);
}

TEST(PositionGroupTrackerTest, PartialClosePosition)
{
  PositionGroupTracker gt;

  PositionId pid = gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0),
                                   Quantity::fromDouble(10.0));
  gt.partialClose(pid, Quantity::fromDouble(4.0), Price::fromDouble(120.0));

  auto* pos = gt.getPosition(pid);
  ASSERT_NE(pos, nullptr);
  EXPECT_FALSE(pos->closed);
  EXPECT_DOUBLE_EQ(pos->quantity.toDouble(), 6.0);
  EXPECT_NEAR(pos->realizedPnl.toDouble(), 80.0, 0.01);
}

TEST(PositionGroupTrackerTest, PartialCloseThenFullClose)
{
  PositionGroupTracker gt;

  PositionId pid = gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0),
                                   Quantity::fromDouble(10.0));
  gt.partialClose(pid, Quantity::fromDouble(6.0), Price::fromDouble(120.0));
  gt.partialClose(pid, Quantity::fromDouble(4.0), Price::fromDouble(130.0));

  auto* pos = gt.getPosition(pid);
  ASSERT_NE(pos, nullptr);
  EXPECT_TRUE(pos->closed);
  EXPECT_NEAR(pos->realizedPnl.toDouble(), 240.0, 0.01);
}

TEST(PositionGroupTrackerTest, ShortPositionPnl)
{
  PositionGroupTracker gt;

  PositionId pid = gt.openPosition(1, 100, Side::SELL, Price::fromDouble(200.0),
                                   Quantity::fromDouble(3.0));
  gt.closePosition(pid, Price::fromDouble(180.0));
  EXPECT_NEAR(gt.totalRealizedPnl().toDouble(), 60.0, 0.01);
}

TEST(PositionGroupTrackerTest, ShortPositionLoss)
{
  PositionGroupTracker gt;

  PositionId pid = gt.openPosition(1, 100, Side::SELL, Price::fromDouble(200.0),
                                   Quantity::fromDouble(3.0));
  gt.closePosition(pid, Price::fromDouble(220.0));
  EXPECT_NEAR(gt.totalRealizedPnl().toDouble(), -60.0, 0.01);
}

TEST(PositionGroupTrackerTest, PruneClosedPositions)
{
  PositionGroupTracker gt;

  gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0), Quantity::fromDouble(1.0));
  PositionId pid2 = gt.openPosition(2, 100, Side::BUY, Price::fromDouble(200.0),
                                    Quantity::fromDouble(1.0));

  gt.closePosition(pid2, Price::fromDouble(210.0));
  gt.pruneClosedPositions();

  EXPECT_EQ(gt.openPositionCount(), 1);
  EXPECT_EQ(gt.getPositionByOrder(2), nullptr);
  EXPECT_NE(gt.getPositionByOrder(1), nullptr);
}

// Regression: prune should clean up stale IDs in groups
TEST(PositionGroupTrackerTest, PruneCleanupGroupRefs)
{
  PositionGroupTracker gt;

  PositionId p1 = gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0),
                                  Quantity::fromDouble(1.0));
  PositionId p2 = gt.openPosition(2, 100, Side::BUY, Price::fromDouble(200.0),
                                  Quantity::fromDouble(1.0));

  GroupId gid = gt.createGroup();
  gt.assignToGroup(p1, gid);
  gt.assignToGroup(p2, gid);

  gt.closePosition(p2, Price::fromDouble(210.0));
  gt.pruneClosedPositions();

  const auto* group = gt.getGroup(gid);
  ASSERT_NE(group, nullptr);
  // After prune, only p1 should remain in group
  EXPECT_EQ(group->positionIds.size(), 1);
  EXPECT_EQ(group->positionIds[0], p1);
}

TEST(PositionGroupTrackerTest, MultipleSymbolNetPosition)
{
  PositionGroupTracker gt;

  gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0), Quantity::fromDouble(3.0));
  gt.openPosition(2, 100, Side::SELL, Price::fromDouble(110.0), Quantity::fromDouble(1.0));
  gt.openPosition(3, 200, Side::SELL, Price::fromDouble(50.0), Quantity::fromDouble(5.0));

  EXPECT_DOUBLE_EQ(gt.netPosition(100).toDouble(), 2.0);
  EXPECT_DOUBLE_EQ(gt.netPosition(200).toDouble(), -5.0);
  EXPECT_EQ(gt.openPositionCount(100), 2);
  EXPECT_EQ(gt.openPositionCount(200), 1);
}

TEST(PositionGroupTrackerTest, GroupPnlRecursive)
{
  PositionGroupTracker gt;

  PositionId p1 = gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0),
                                  Quantity::fromDouble(1.0));
  PositionId p2 = gt.openPosition(2, 100, Side::BUY, Price::fromDouble(200.0),
                                  Quantity::fromDouble(1.0));
  PositionId p3 = gt.openPosition(3, 100, Side::BUY, Price::fromDouble(300.0),
                                  Quantity::fromDouble(1.0));

  GroupId parent = gt.createGroup();
  GroupId child = gt.createGroup(parent);

  gt.assignToGroup(p1, parent);
  gt.assignToGroup(p2, child);
  gt.assignToGroup(p3, child);

  gt.closePosition(p1, Price::fromDouble(150.0));
  gt.closePosition(p2, Price::fromDouble(180.0));
  gt.closePosition(p3, Price::fromDouble(350.0));

  EXPECT_NEAR(gt.groupRealizedPnl(child).toDouble(), 30.0, 0.01);
  EXPECT_NEAR(gt.groupRealizedPnl(parent).toDouble(), 80.0, 0.01);
}

TEST(PositionGroupTrackerTest, ForEachOpen)
{
  PositionGroupTracker gt;

  gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0), Quantity::fromDouble(1.0));
  PositionId p2 = gt.openPosition(2, 100, Side::SELL, Price::fromDouble(200.0),
                                  Quantity::fromDouble(2.0));
  gt.openPosition(3, 200, Side::BUY, Price::fromDouble(300.0), Quantity::fromDouble(3.0));

  gt.closePosition(p2, Price::fromDouble(190.0));

  int count = 0;
  gt.forEachOpen([&count](const IndividualPosition& pos)
                 {
    (void)pos;
    ++count; });
  EXPECT_EQ(count, 2);
}

TEST(PositionReconcilerTest, DetectsMismatch)
{
  PositionReconciler reconciler;

  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  auto mismatches = reconciler.reconcile(exchange,
                                         [](SymbolId) -> std::pair<Quantity, Price>
                                         {
                                           return {Quantity::fromDouble(3.0), Price::fromDouble(100.0)};
                                         });

  ASSERT_EQ(mismatches.size(), 1);
  EXPECT_DOUBLE_EQ(mismatches[0].exchangeQuantity.toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(mismatches[0].localQuantity.toDouble(), 3.0);
  EXPECT_DOUBLE_EQ(mismatches[0].quantityDiff().toDouble(), 2.0);
}

TEST(PositionReconcilerTest, NoMismatchWhenMatching)
{
  PositionReconciler reconciler;

  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  auto mismatches = reconciler.reconcile(exchange,
                                         [](SymbolId) -> std::pair<Quantity, Price>
                                         {
                                           return {Quantity::fromDouble(5.0), Price::fromDouble(100.0)};
                                         });

  EXPECT_TRUE(mismatches.empty());
}

TEST(PositionReconcilerTest, ReconcileAndApply)
{
  PositionReconciler reconciler;
  reconciler.setMismatchHandler([](const PositionMismatch&)
                                { return ReconcileAction::ACCEPT_EXCHANGE; });

  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  Quantity adjustedQty{};
  Price adjustedPrice{};

  reconciler.reconcileAndApply(exchange, [](SymbolId) -> std::pair<Quantity, Price>
                               { return {Quantity::fromDouble(3.0), Price::fromDouble(95.0)}; }, [&](SymbolId, Quantity qty, Price price)
                               {
      adjustedQty = qty;
      adjustedPrice = price; });

  EXPECT_DOUBLE_EQ(adjustedQty.toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(adjustedPrice.toDouble(), 100.0);
}

TEST(PositionReconcilerTest, FlagOnlyDoesNotAdjust)
{
  PositionReconciler reconciler;
  reconciler.setMismatchHandler([](const PositionMismatch&)
                                { return ReconcileAction::FLAG_ONLY; });

  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  bool adjustCalled = false;

  reconciler.reconcileAndApply(exchange, [](SymbolId) -> std::pair<Quantity, Price>
                               { return {Quantity::fromDouble(3.0), Price::fromDouble(95.0)}; }, [&](SymbolId, Quantity, Price)
                               { adjustCalled = true; });

  EXPECT_FALSE(adjustCalled);
}

TEST(PositionReconcilerTest, AcceptLocalSkipsAdjust)
{
  PositionReconciler reconciler;
  reconciler.setMismatchHandler([](const PositionMismatch&)
                                { return ReconcileAction::ACCEPT_LOCAL; });

  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  bool adjustCalled = false;

  auto mismatches = reconciler.reconcileAndApply(exchange, [](SymbolId) -> std::pair<Quantity, Price>
                                                 { return {Quantity::fromDouble(3.0), Price::fromDouble(95.0)}; }, [&](SymbolId, Quantity, Price)
                                                 { adjustCalled = true; });

  EXPECT_FALSE(adjustCalled);
  EXPECT_EQ(mismatches.size(), 1);
}

TEST(PositionReconcilerTest, MultipleSymbolReconciliation)
{
  PositionReconciler reconciler;
  reconciler.setMismatchHandler([](const PositionMismatch&)
                                { return ReconcileAction::ACCEPT_EXCHANGE; });

  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
      {200, Quantity::fromDouble(3.0), Price::fromDouble(50.0)},
      {300, Quantity::fromDouble(10.0), Price::fromDouble(200.0)},
  };

  std::vector<std::pair<SymbolId, Quantity>> adjustments;

  reconciler.reconcileAndApply(exchange, [](SymbolId sym) -> std::pair<Quantity, Price>
                               {
      if (sym == 100){ return {Quantity::fromDouble(5.0), Price::fromDouble(100.0)};
}
      if (sym == 200){ return {Quantity::fromDouble(1.0), Price::fromDouble(50.0)};
}
      return {Quantity::fromDouble(8.0), Price::fromDouble(200.0)}; }, [&](SymbolId sym, Quantity qty, Price)
                               { adjustments.emplace_back(sym, qty); });

  ASSERT_EQ(adjustments.size(), 2);
  EXPECT_EQ(adjustments[0].first, 200u);
  EXPECT_DOUBLE_EQ(adjustments[0].second.toDouble(), 3.0);
  EXPECT_EQ(adjustments[1].first, 300u);
  EXPECT_DOUBLE_EQ(adjustments[1].second.toDouble(), 10.0);
}

TEST(PositionReconcilerTest, ReconcileWithRealTracker)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET, CostBasisMethod::FIFO};
  PositionReconciler reconciler;

  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 3.0));

  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  auto mismatches = reconciler.reconcile(exchange,
                                         [&](SymbolId sym) -> std::pair<Quantity, Price>
                                         {
                                           return {tracker.getPosition(sym), Price::fromDouble(100.0)};
                                         });

  ASSERT_EQ(mismatches.size(), 1);
  EXPECT_DOUBLE_EQ(mismatches[0].localQuantity.toDouble(), 3.0);
  EXPECT_DOUBLE_EQ(mismatches[0].exchangeQuantity.toDouble(), 5.0);
}

// Regression: bidirectional reconciliation detects local-only positions
TEST(PositionReconcilerTest, BidirectionalDetectsLocalOnly)
{
  PositionReconciler reconciler;

  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  auto mismatches = reconciler.reconcile(exchange,
                                         [](SymbolId sym) -> std::pair<Quantity, Price>
                                         {
                                           if (sym == 100)
                                           {
                                             return {Quantity::fromDouble(5.0), Price::fromDouble(100.0)};
                                           }
                                           if (sym == 200)
                                           {
                                             return {Quantity::fromDouble(3.0), Price::fromDouble(50.0)};
                                           }
                                           return {Quantity{}, Price{}};
                                         },
                                         {100, 200});

  ASSERT_EQ(mismatches.size(), 1);
  EXPECT_EQ(mismatches[0].symbol, 200u);
  EXPECT_DOUBLE_EQ(mismatches[0].localQuantity.toDouble(), 3.0);
  EXPECT_DOUBLE_EQ(mismatches[0].exchangeQuantity.toDouble(), 0.0);
}

// Regression: bidirectional with zero local position is not a mismatch
TEST(PositionReconcilerTest, BidirectionalIgnoresZeroLocalPositions)
{
  PositionReconciler reconciler;

  std::vector<ExchangePosition> exchange = {};

  auto mismatches = reconciler.reconcile(exchange,
                                         [](SymbolId) -> std::pair<Quantity, Price>
                                         {
                                           return {Quantity{}, Price{}};
                                         },
                                         {100, 200});

  EXPECT_TRUE(mismatches.empty());
}

TEST(MultiExecutionListenerTest, DistributesToAllTrackers)
{
  MultiExecutionListener dist{0};
  auto net = std::make_shared<MultiModePositionTracker>(1, PositionAggregationMode::NET);
  auto perSide = std::make_shared<MultiModePositionTracker>(2, PositionAggregationMode::PER_SIDE);
  auto grouped = std::make_shared<MultiModePositionTracker>(3, PositionAggregationMode::GROUPED);

  dist.addListener(net.get());
  dist.addListener(perSide.get());
  dist.addListener(grouped.get());

  dist.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));

  EXPECT_DOUBLE_EQ(net->getPosition(100).toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(perSide->getLongPosition(100).toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(grouped->getPosition(100).toDouble(), 5.0);
}

TEST(MultiExecutionListenerTest, PartialFillDistributed)
{
  MultiExecutionListener dist{0};
  auto net = std::make_shared<MultiModePositionTracker>(1, PositionAggregationMode::NET);
  auto grouped = std::make_shared<MultiModePositionTracker>(2, PositionAggregationMode::GROUPED);
  dist.addListener(net.get());
  dist.addListener(grouped.get());

  auto order = makeOrder(1, 100, Side::BUY, 100.0, 10.0);
  dist.onOrderPartiallyFilled(order, Quantity::fromDouble(3.0));
  dist.onOrderPartiallyFilled(order, Quantity::fromDouble(4.0));

  EXPECT_DOUBLE_EQ(net->getPosition(100).toDouble(), 7.0);
  EXPECT_DOUBLE_EQ(grouped->getPosition(100).toDouble(), 7.0);
}

TEST(MultiExecutionListenerTest, CancelDistributed)
{
  MultiExecutionListener dist{0};
  auto grouped = std::make_shared<MultiModePositionTracker>(1, PositionAggregationMode::GROUPED);
  dist.addListener(grouped.get());

  auto order = makeOrder(1, 100, Side::BUY, 100.0, 5.0);
  dist.onOrderFilled(order);

  EXPECT_EQ(grouped->groups().openPositionCount(), 1);
}

TEST(MultiExecutionListenerTest, ConsistentPnlAcrossTrackers)
{
  MultiExecutionListener dist{0};
  auto net = std::make_shared<MultiModePositionTracker>(1, PositionAggregationMode::NET);
  auto perSide = std::make_shared<MultiModePositionTracker>(2, PositionAggregationMode::PER_SIDE);
  dist.addListener(net.get());
  dist.addListener(perSide.get());

  dist.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 10.0));
  dist.onOrderFilled(makeOrder(2, 100, Side::SELL, 110.0, 10.0, true));

  double netPnl = net->getRealizedPnl(100).toDouble();
  double perSidePnl = perSide->getRealizedPnl(100).toDouble();
  EXPECT_NEAR(netPnl, 100.0, 0.01);
  EXPECT_NEAR(perSidePnl, 100.0, 0.01);
}

TEST(SnapshotTest, NetModeSnapshot)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));

  auto snap = tracker.snapshot(100);
  EXPECT_DOUBLE_EQ(snap.longQty.toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(snap.shortQty.toDouble(), 0.0);
  EXPECT_NEAR(snap.longAvgEntry.toDouble(), 100.0, 0.01);
  EXPECT_DOUBLE_EQ(snap.netQty().toDouble(), 5.0);
}

TEST(SnapshotTest, NetModeShortSnapshot)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  tracker.onOrderFilled(makeOrder(1, 100, Side::SELL, 200.0, 3.0));

  auto snap = tracker.snapshot(100);
  EXPECT_DOUBLE_EQ(snap.longQty.toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(snap.shortQty.toDouble(), 3.0);
  EXPECT_NEAR(snap.shortAvgEntry.toDouble(), 200.0, 0.01);
  EXPECT_DOUBLE_EQ(snap.netQty().toDouble(), -3.0);
}

TEST(SnapshotTest, PerSideSnapshot)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::PER_SIDE};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 200.0, 3.0));

  auto snap = tracker.snapshot(100);
  EXPECT_DOUBLE_EQ(snap.longQty.toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(snap.shortQty.toDouble(), 3.0);
  EXPECT_NEAR(snap.longAvgEntry.toDouble(), 100.0, 0.01);
  EXPECT_NEAR(snap.shortAvgEntry.toDouble(), 200.0, 0.01);
  EXPECT_DOUBLE_EQ(snap.netQty().toDouble(), 2.0);
}

TEST(SnapshotTest, GroupedSnapshot)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::GROUPED};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 3.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 200.0, 1.0));

  auto snap = tracker.snapshot(100);
  EXPECT_DOUBLE_EQ(snap.longQty.toDouble(), 3.0);
  EXPECT_DOUBLE_EQ(snap.shortQty.toDouble(), 1.0);
  EXPECT_NEAR(snap.longAvgEntry.toDouble(), 100.0, 0.01);
  EXPECT_NEAR(snap.shortAvgEntry.toDouble(), 200.0, 0.01);
}

TEST(SnapshotTest, UnrealizedPnlLong)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 10.0));

  auto upnl = tracker.getUnrealizedPnl(100, Price::fromDouble(120.0));
  EXPECT_NEAR(upnl.toDouble(), 200.0, 0.01);
}

TEST(SnapshotTest, UnrealizedPnlShort)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  tracker.onOrderFilled(makeOrder(1, 100, Side::SELL, 200.0, 5.0));

  auto upnl = tracker.getUnrealizedPnl(100, Price::fromDouble(180.0));
  EXPECT_NEAR(upnl.toDouble(), 100.0, 0.01);
}

TEST(SnapshotTest, UnrealizedPnlPerSideBothSides)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::PER_SIDE};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  tracker.onOrderFilled(makeOrder(2, 100, Side::SELL, 200.0, 3.0));

  // Price at 150: long profit = (150-100)*5=250, short loss = (200-150)*3=150
  auto upnl = tracker.getUnrealizedPnl(100, Price::fromDouble(150.0));
  EXPECT_NEAR(upnl.toDouble(), 400.0, 0.01);
}

TEST(SnapshotTest, EmptyPositionSnapshotIsZero)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  auto snap = tracker.snapshot(100);
  EXPECT_DOUBLE_EQ(snap.longQty.toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(snap.shortQty.toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(snap.unrealizedPnl(Price::fromDouble(100.0)).toDouble(), 0.0);
}

TEST(GroupedQueryTest, GetOpenPositionsBySymbol)
{
  PositionGroupTracker gt;
  gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0), Quantity::fromDouble(1.0));
  gt.openPosition(2, 100, Side::SELL, Price::fromDouble(200.0), Quantity::fromDouble(2.0));
  gt.openPosition(3, 200, Side::BUY, Price::fromDouble(50.0), Quantity::fromDouble(3.0));
  PositionId p4 = gt.openPosition(4, 100, Side::BUY, Price::fromDouble(150.0), Quantity::fromDouble(1.0));
  gt.closePosition(p4, Price::fromDouble(160.0));

  auto positions = gt.getOpenPositions(100);
  EXPECT_EQ(positions.size(), 2);

  auto positions200 = gt.getOpenPositions(200);
  EXPECT_EQ(positions200.size(), 1);
}

TEST(ReconcileConvenienceTest, ReconcileWithTracker)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 3.0));

  PositionReconciler reconciler;
  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  auto mismatches = reconcileWith(reconciler, tracker, exchange);
  ASSERT_EQ(mismatches.size(), 1);
  EXPECT_DOUBLE_EQ(mismatches[0].localQuantity.toDouble(), 3.0);
  EXPECT_DOUBLE_EQ(mismatches[0].exchangeQuantity.toDouble(), 5.0);
}

TEST(ReconcileConvenienceTest, ReconcileWithMatchingPositions)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));

  PositionReconciler reconciler;
  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  auto mismatches = reconcileWith(reconciler, tracker, exchange);
  EXPECT_TRUE(mismatches.empty());
}

TEST(ResetTest, NetModeReset)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 5.0);

  tracker.reset();
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(tracker.getRealizedPnl(100).toDouble(), 0.0);
}

TEST(ResetTest, PerSideReset)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::PER_SIDE};
  tracker.openLong(100, Price::fromDouble(100.0), Quantity::fromDouble(5.0));
  tracker.openShort(100, Price::fromDouble(200.0), Quantity::fromDouble(3.0));

  tracker.reset();
  EXPECT_DOUBLE_EQ(tracker.getLongPosition(100).toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(tracker.getShortPosition(100).toDouble(), 0.0);
}

TEST(ResetTest, GroupedReset)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::GROUPED};
  tracker.openLong(100, Price::fromDouble(100.0), Quantity::fromDouble(1.0), 10);
  tracker.openLong(100, Price::fromDouble(200.0), Quantity::fromDouble(1.0), 10);

  tracker.reset();
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 0.0);
  EXPECT_EQ(tracker.groups().openPositionCount(), 0);
}

TEST(PositionChangeCallbackTest, FiresOnFill)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};

  SymbolId capturedSym{};
  double capturedQty = 0;
  tracker.onPositionChange([&](SymbolId sym, const MultiModePositionTracker::PositionSnapshot& snap)
                           {
    capturedSym = sym;
    capturedQty = snap.netQty().toDouble(); });

  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  EXPECT_EQ(capturedSym, 100u);
  EXPECT_DOUBLE_EQ(capturedQty, 5.0);
}

TEST(PositionChangeCallbackTest, FiresOnExplicitFill)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::PER_SIDE};

  int callCount = 0;
  tracker.onPositionChange([&](SymbolId, const MultiModePositionTracker::PositionSnapshot&)
                           { ++callCount; });

  tracker.openLong(100, Price::fromDouble(100.0), Quantity::fromDouble(5.0));
  tracker.closeLong(100, Price::fromDouble(110.0), Quantity::fromDouble(5.0));
  EXPECT_EQ(callCount, 2);
}

TEST(PositionChangeCallbackTest, NoCallbackNoCrash)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 5.0));
  EXPECT_DOUBLE_EQ(tracker.getPosition(100).toDouble(), 5.0);
}

TEST(GroupUnrealizedPnlTest, SingleGroup)
{
  PositionGroupTracker gt;
  PositionId p1 = gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0),
                                  Quantity::fromDouble(5.0));
  PositionId p2 = gt.openPosition(2, 100, Side::SELL, Price::fromDouble(200.0),
                                  Quantity::fromDouble(3.0));

  GroupId gid = gt.createGroup();
  gt.assignToGroup(p1, gid);
  gt.assignToGroup(p2, gid);

  // Price at 150: long uPnL = (150-100)*5 = 250, short uPnL = -(150-200)*3 = 150
  auto upnl = gt.groupUnrealizedPnl(gid, Price::fromDouble(150.0));
  EXPECT_NEAR(upnl.toDouble(), 400.0, 0.01);
}

TEST(GroupUnrealizedPnlTest, RecursiveSubGroups)
{
  PositionGroupTracker gt;
  PositionId p1 = gt.openPosition(1, 100, Side::BUY, Price::fromDouble(100.0),
                                  Quantity::fromDouble(1.0));
  PositionId p2 = gt.openPosition(2, 100, Side::BUY, Price::fromDouble(200.0),
                                  Quantity::fromDouble(1.0));

  GroupId parent = gt.createGroup();
  GroupId child = gt.createGroup(parent);
  gt.assignToGroup(p1, parent);
  gt.assignToGroup(p2, child);

  // Price 150: p1 uPnL = 50, p2 uPnL = -50. Total = 0
  auto upnl = gt.groupUnrealizedPnl(parent, Price::fromDouble(150.0));
  EXPECT_NEAR(upnl.toDouble(), 0.0, 0.01);
}

TEST(LockedGroupsTest, ThreadSafeAccess)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::GROUPED};
  tracker.openLong(100, Price::fromDouble(100.0), Quantity::fromDouble(5.0));

  auto positions = tracker.lockedGroups()->getOpenPositions(100);
  EXPECT_EQ(positions.size(), 1);
}

TEST(AtomicReconcileTest, ReconcileMethodOnTracker)
{
  MultiModePositionTracker tracker{1, PositionAggregationMode::NET};
  tracker.onOrderFilled(makeOrder(1, 100, Side::BUY, 100.0, 3.0));

  PositionReconciler reconciler;
  std::vector<ExchangePosition> exchange = {
      {100, Quantity::fromDouble(5.0), Price::fromDouble(100.0)},
  };

  auto mismatches = tracker.reconcile(reconciler, exchange);
  ASSERT_EQ(mismatches.size(), 1);
  EXPECT_DOUBLE_EQ(mismatches[0].localQuantity.toDouble(), 3.0);
  EXPECT_DOUBLE_EQ(mismatches[0].exchangeQuantity.toDouble(), 5.0);
}
