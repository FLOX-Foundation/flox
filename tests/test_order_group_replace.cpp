/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/execution/order_group.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;
constexpr SymbolId ETH = 2;
constexpr uint8_t BUY = 0;
constexpr uint8_t SELL = 1;
}  // namespace

TEST(OrderGroupReplace, AcceptedSwapsOrderIdKeepsLegState)
{
  OrderGroup g(/*parentSignalId=*/1, OrderGroupPolicy::OneSided);
  auto leg = g.addLimitLeg(BTC, BUY, Price::fromDouble(50000.0), Quantity::fromDouble(0.1));
  g.recordSubmit(leg, /*orderId=*/100);
  EXPECT_EQ(g.leg(leg).orderId, 100u);
  EXPECT_EQ(g.leg(leg).state, LegState::Submitted);

  g.recordReplaceAccepted(leg, /*newOrderId=*/200);
  EXPECT_EQ(g.leg(leg).orderId, 200u);
  EXPECT_EQ(g.leg(leg).state, LegState::Submitted);
}

TEST(OrderGroupReplace, AcceptedPreservesPartiallyFilledState)
{
  OrderGroup g(2, OrderGroupPolicy::AllOrNothing);
  auto leg = g.addLimitLeg(BTC, BUY, Price::fromDouble(50000.0), Quantity::fromDouble(1.0));
  g.recordSubmit(leg, 100);
  g.recordFill(leg, Quantity::fromDouble(0.3));
  EXPECT_EQ(g.leg(leg).state, LegState::PartiallyFilled);

  g.recordReplaceAccepted(leg, 201);
  EXPECT_EQ(g.leg(leg).orderId, 201u);
  EXPECT_EQ(g.leg(leg).state, LegState::PartiallyFilled);
  EXPECT_EQ(g.leg(leg).filledQty.toDouble(), 0.3);
}

TEST(OrderGroupReplace, RejectedKeepsExistingOrderId)
{
  OrderGroup g(3, OrderGroupPolicy::OneSided);
  auto leg = g.addLimitLeg(BTC, BUY, Price::fromDouble(50000.0), Quantity::fromDouble(0.1));
  g.recordSubmit(leg, 100);

  g.recordReplaceRejected(leg);
  EXPECT_EQ(g.leg(leg).orderId, 100u);
  EXPECT_EQ(g.leg(leg).state, LegState::Submitted);
}

TEST(OrderGroupReplace, AcceptedAfterTerminalIsNoop)
{
  OrderGroup g(4, OrderGroupPolicy::OneSided);
  auto leg = g.addLimitLeg(BTC, BUY, Price::fromDouble(50000.0), Quantity::fromDouble(0.1));
  g.recordSubmit(leg, 100);
  g.recordFill(leg, Quantity::fromDouble(0.1));
  EXPECT_EQ(g.leg(leg).state, LegState::Filled);

  // Late replace ack arrives after the leg already filled — same race
  // the simulator surfaces as REPLACE_REJECTED. The group must not
  // overwrite the order id of a terminal leg, otherwise downstream
  // post-fill bookkeeping points at a phantom id.
  g.recordReplaceAccepted(leg, 999);
  EXPECT_EQ(g.leg(leg).orderId, 100u);
  EXPECT_EQ(g.leg(leg).state, LegState::Filled);
}

TEST(OrderGroupReplace, FindLegByOrderIdMatches)
{
  OrderGroup g(5, OrderGroupPolicy::OneSided);
  auto a = g.addLimitLeg(BTC, BUY, Price::fromDouble(50000.0), Quantity::fromDouble(0.1));
  auto b = g.addLimitLeg(ETH, SELL, Price::fromDouble(3000.0), Quantity::fromDouble(1.0));
  g.recordSubmit(a, 100);
  g.recordSubmit(b, 200);

  EXPECT_EQ(g.findLegByOrderId(100), a);
  EXPECT_EQ(g.findLegByOrderId(200), b);
  EXPECT_FALSE(g.findLegByOrderId(999).has_value());
}

TEST(OrderGroupReplace, FindLegFollowsReplacedId)
{
  OrderGroup g(6, OrderGroupPolicy::OneSided);
  auto leg = g.addLimitLeg(BTC, BUY, Price::fromDouble(50000.0), Quantity::fromDouble(0.1));
  g.recordSubmit(leg, 100);
  g.recordReplaceAccepted(leg, 201);

  // The strategy routes the next replace ack through findLegByOrderId
  // — the new id, not the old one, must resolve back to the leg.
  EXPECT_FALSE(g.findLegByOrderId(100).has_value());
  EXPECT_EQ(g.findLegByOrderId(201), leg);
}

TEST(OrderGroupReplace, OneSidedFillOnReplacedLegStillCancelsOther)
{
  OrderGroup g(7, OrderGroupPolicy::OneSided);
  auto a = g.addLimitLeg(BTC, BUY, Price::fromDouble(50000.0), Quantity::fromDouble(0.1));
  auto b = g.addLimitLeg(ETH, SELL, Price::fromDouble(3000.0), Quantity::fromDouble(1.0));
  g.recordSubmit(a, 100);
  g.recordSubmit(b, 200);

  g.recordReplaceAccepted(a, 101);
  EXPECT_TRUE(g.recommendedActions().empty());

  // Fill on the replaced leg should still trigger the OneSided
  // cancellation of the other leg — the policy state machine sees
  // the leg slot, not the swapped exchange id.
  g.recordFill(a, Quantity::fromDouble(0.1));
  auto actions = g.recommendedActions();
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].kind, OrderGroupAction::Kind::CancelLeg);
  EXPECT_EQ(actions[0].legIndex, b);
  EXPECT_EQ(actions[0].orderId, 200u);
}

TEST(OrderGroupReplace, AllOrNothingRevertUsesReplacedId)
{
  // Bracket-style: entry leg + protective stop. After entry fills,
  // the stop is replaced to tighten the price. If the stop then
  // fails (e.g. the venue rejects), AllOrNothing wants to revert
  // the entry fill. The recommendedActions for the still-open stop
  // leg must reference the *replaced* order id so the cancel routes.
  OrderGroup g(8, OrderGroupPolicy::AllOrNothing);
  auto entry = g.addMarketLeg(BTC, BUY, Quantity::fromDouble(1.0));
  auto stop = g.addLimitLeg(BTC, SELL, Price::fromDouble(49000.0), Quantity::fromDouble(1.0));

  g.recordSubmit(entry, 100);
  g.recordSubmit(stop, 200);
  g.recordFill(entry, Quantity::fromDouble(1.0));
  g.recordReplaceAccepted(stop, 201);

  // Now a separate leg fails (simulate the entry getting reverted
  // upstream is not the case here; let's fail the stop instead).
  g.recordFailure(stop);

  auto actions = g.recommendedActions();
  // Should recommend reverting the entry fill (opposite side market).
  bool sawRevert = false;
  for (const auto& a : actions)
  {
    if (a.kind == OrderGroupAction::Kind::RevertLeg && a.legIndex == entry)
    {
      sawRevert = true;
      EXPECT_EQ(a.side, SELL);
      EXPECT_EQ(a.qty.toDouble(), 1.0);
    }
  }
  EXPECT_TRUE(sawRevert);
}
