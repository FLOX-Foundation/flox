/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/execution/events/order_event.h"
#include "flox/position/multi_mode_position_tracker.h"
#include "flox/position/position_tracker.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

Order marketOrder(OrderId id, Side side, double qty)
{
  Order o;
  o.id = id;
  o.symbol = BTC;
  o.side = side;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(qty);
  // A market order carries no price of its own -- the venue decides it.
  return o;
}

OrderEvent fillEvent(OrderEventStatus status, const Order& order, double fillQty,
                     double fillPrice)
{
  OrderEvent ev;
  ev.status = status;
  ev.order = order;
  ev.fillQty = Quantity::fromDouble(fillQty);
  ev.fillPrice = Price::fromDouble(fillPrice);
  return ev;
}
}  // namespace

// The price a fill happened at is the one piece of information a position
// tracker cannot reconstruct. For a market order the order itself carries no
// price, so dropping the fill price on dispatch leaves the tracker with a zero
// entry and a zero realized result.
TEST(FillPriceDispatch, PositionTrackerLearnsTheMarketFillPrice)
{
  PositionTracker tracker(1);

  const Order buy = marketOrder(1, Side::BUY, 2.0);
  fillEvent(OrderEventStatus::FILLED, buy, 2.0, 100.0).dispatchTo(tracker);

  EXPECT_DOUBLE_EQ(tracker.getPosition(BTC).toDouble(), 2.0);
  EXPECT_DOUBLE_EQ(tracker.getAvgEntryPrice(BTC).toDouble(), 100.0);

  const Order sell = marketOrder(2, Side::SELL, 2.0);
  fillEvent(OrderEventStatus::FILLED, sell, 2.0, 110.0).dispatchTo(tracker);

  EXPECT_DOUBLE_EQ(tracker.getPosition(BTC).toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(tracker.getRealizedPnl(BTC).toDouble(), 20.0);
}

TEST(FillPriceDispatch, PositionTrackerLearnsPartialFillPrices)
{
  PositionTracker tracker(1);

  const Order buy = marketOrder(1, Side::BUY, 4.0);
  fillEvent(OrderEventStatus::PARTIALLY_FILLED, buy, 2.0, 100.0).dispatchTo(tracker);
  fillEvent(OrderEventStatus::PARTIALLY_FILLED, buy, 2.0, 104.0).dispatchTo(tracker);

  EXPECT_DOUBLE_EQ(tracker.getPosition(BTC).toDouble(), 4.0);
  EXPECT_DOUBLE_EQ(tracker.getAvgEntryPrice(BTC).toDouble(), 102.0);
}

TEST(FillPriceDispatch, MultiModeTrackerLearnsTheMarketFillPrice)
{
  MultiModePositionTracker tracker(1, PositionAggregationMode::NET);

  const Order buy = marketOrder(1, Side::BUY, 2.0);
  fillEvent(OrderEventStatus::FILLED, buy, 2.0, 100.0).dispatchTo(tracker);

  const auto snap = tracker.snapshot(BTC);
  EXPECT_DOUBLE_EQ(snap.netQty().toDouble(), 2.0);
  EXPECT_DOUBLE_EQ(snap.longAvgEntry.toDouble(), 100.0);
}

namespace
{
// A listener written before the fill price was carried through still only
// overrides the two-argument form. It has to keep receiving those calls.
class LegacyListener : public IOrderExecutionListener
{
 public:
  LegacyListener() : IOrderExecutionListener(7) {}

  void onOrderPartiallyFilled(const Order&, Quantity qty) override
  {
    partialQty += qty.toDouble();
    ++partials;
  }

  void onOrderFilled(const Order&) override { ++fulls; }

  double partialQty{0.0};
  int partials{0};
  int fulls{0};
};
}  // namespace

TEST(FillPriceDispatch, ListenersOnTheOlderSignatureStillGetTheirCalls)
{
  LegacyListener listener;

  const Order buy = marketOrder(1, Side::BUY, 4.0);
  fillEvent(OrderEventStatus::PARTIALLY_FILLED, buy, 1.5, 100.0).dispatchTo(listener);
  fillEvent(OrderEventStatus::FILLED, buy, 2.5, 101.0).dispatchTo(listener);

  EXPECT_EQ(listener.partials, 1);
  EXPECT_DOUBLE_EQ(listener.partialQty, 1.5);
  EXPECT_EQ(listener.fulls, 1);
}
