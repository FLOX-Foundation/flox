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
  if (bidQty > 0.0)
  {
    bids.emplace_back(Price::fromDouble(bid), Quantity::fromDouble(bidQty));
  }
  if (askQty > 0.0)
  {
    asks.emplace_back(Price::fromDouble(ask), Quantity::fromDouble(askQty));
  }
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
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::SELL;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

// The same resting order, reached through a trigger instead of submitted
// live: a take-profit limit posts its limit price once the market has
// crossed its trigger, and from that moment it is a resting limit like
// any other.
Order takeProfitLimitSell(OrderId id, SymbolId sym, double trigger, double price, double qty)
{
  Order o = limitSell(id, sym, price, qty);
  o.type = OrderType::TAKE_PROFIT_LIMIT;
  o.triggerPrice = Price::fromDouble(trigger);
  return o;
}

const char* positionName(MarketPosition p)
{
  switch (p)
  {
    case MarketPosition::Unknown:
      return "Unknown";
    case MarketPosition::Best:
      return "Best";
    case MarketPosition::BehindBest:
      return "BehindBest";
    case MarketPosition::MidSpread:
      return "MidSpread";
    case MarketPosition::LevelEmpty:
      return "LevelEmpty";
    case MarketPosition::Crossed:
      return "Crossed";
  }
  return "?";
}

const OrderEvent* findMarketPositionEvent(const std::vector<OrderEvent>& events,
                                          OrderId id)
{
  const OrderEvent* last = nullptr;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::MARKET_POSITION_CHANGED && ev.order.id == id)
    {
      last = &ev;
    }
  }
  return last;
}
}  // namespace

TEST(MarketPosition, BestWhenOrderSitsAtBestBid)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));

  // Trigger another book update to fire market-position recompute.
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  const OrderEvent* ev = findMarketPositionEvent(events, 1);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->marketPosition, MarketPosition::Best);
}

TEST(MarketPosition, BehindBestWhenBestBidMovesAhead)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));

  // New best bid jumps above our 100.0.
  pushBook(exec, 1, 100.5, 5.0, 101.0, 5.0);

  const OrderEvent* ev = findMarketPositionEvent(events, 1);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->marketPosition, MarketPosition::BehindBest);
}

TEST(MarketPosition, MidSpreadWhenOrderPriceBetweenBidAndAsk)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // Empty bid side, ask at 110. A buy limit at 105 is mid-spread.
  pushBook(exec, 1, 0.0, 0.0, 110.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 105.0, 1.0));

  // Add a bid below, ask above — still mid-spread.
  pushBook(exec, 1, 100.0, 5.0, 110.0, 5.0);

  const OrderEvent* ev = findMarketPositionEvent(events, 1);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->marketPosition, MarketPosition::MidSpread);
}

TEST(MarketPosition, EmitsOnlyOnCategoricalTransition)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));

  // Three more identical book ticks → no extra MARKET_POSITION_CHANGED.
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  size_t mpEvents = 0;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::MARKET_POSITION_CHANGED && ev.order.id == 1)
    {
      ++mpEvents;
    }
  }
  EXPECT_EQ(mpEvents, 1u);
}

TEST(MarketPosition, LevelEmptyWhenAloneAtPrice)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // Order arrives at a price where no other resting qty exists.
  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  // Trigger recompute.
  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);

  const OrderEvent* ev = findMarketPositionEvent(events, 1);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->marketPosition, MarketPosition::LevelEmpty);
}

TEST(MarketPosition, NotLevelEmptyWhenOthersRestAtSamePrice)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // 5 lots of OTHER resting qty at our price.
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  const OrderEvent* ev = findMarketPositionEvent(events, 1);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->marketPosition, MarketPosition::Best);
}

TEST(MarketPosition, TransitionsToLevelEmptyWhenOthersCancel)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  // Start: 5 lots ahead, we are Best (but not LevelEmpty).
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  // Others cancel: level drops to zero.
  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);

  bool sawBest = false;
  bool sawLevelEmpty = false;
  for (const auto& ev : events)
  {
    if (ev.status != OrderEventStatus::MARKET_POSITION_CHANGED)
    {
      continue;
    }
    if (ev.marketPosition == MarketPosition::Best)
    {
      sawBest = true;
    }
    if (ev.marketPosition == MarketPosition::LevelEmpty)
    {
      sawLevelEmpty = true;
    }
  }
  EXPECT_TRUE(sawBest);
  EXPECT_TRUE(sawLevelEmpty);
}

TEST(MarketPosition, DistanceToBestTicksReflectsGap)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  pushBook(exec, 1, 100.5, 5.0, 101.0, 5.0);

  const OrderEvent* ev = findMarketPositionEvent(events, 1);
  ASSERT_NE(ev, nullptr);
  // best bid - our price = +0.5 in raw units (positive = behind best).
  EXPECT_GT(ev->distanceToBestTicks, 0);
}

// A fired take-profit limit is a resting limit, and the market-position
// stream is how a strategy watches one: where it sits relative to best,
// how far off it is, when that changes. A take-profit exit that has armed
// and is now waiting is the order a strategy most wants to watch, and
// what it reports is wrong.
//
// The cause is fillsAsLimit (src/backtest/simulated_executor.cpp:41),
// which lists LIMIT and STOP_LIMIT and gates all three of its call sites
// -- the queue tracker's registration, driveQueueFromBarStep and
// maybeEmitMarketPositionChanges. Unregistered by the queue tracker, the
// order's level looks empty to computeMarketPosition no matter what rests
// there. The fill paths gate on !fillsAsMarket instead, which is why a
// take-profit limit still fills correctly and only its reporting is
// wrong -- and why nothing caught this.
//
// Two orders at one price, one of each kind, so the assertion does not
// depend on what the position should be: whatever the plain limit
// reports, the take-profit limit resting beside it has to report too.
TEST(MarketPosition, FiredTakeProfitLimitReportsLikeAPlainLimit)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitSell(1, 1, 101.0, 1.0));
  exec.submitOrder(takeProfitLimitSell(2, 1, 100.5, 101.0, 1.0));

  // A print at 100.6 crosses the trigger without reaching either resting
  // sell at 101.0, so order 2 arms and the two rest side by side.
  exec.onTrade(1, Price::fromDouble(100.6), Quantity::fromDouble(1.0), true);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  const OrderEvent* plain = findMarketPositionEvent(events, 1);
  ASSERT_NE(plain, nullptr) << "the plain limit reported no position -- the control is broken";

  const OrderEvent* takeProfit = findMarketPositionEvent(events, 2);
  ASSERT_NE(takeProfit, nullptr)
      << "the fired take-profit limit rests at the same price as the plain limit, which "
         "reported "
      << positionName(plain->marketPosition) << ", and reported no position at all";
  EXPECT_EQ(takeProfit->marketPosition, plain->marketPosition)
      << "two sells resting at 101.0: the plain limit reports "
      << positionName(plain->marketPosition) << ", the fired take-profit limit reports "
      << positionName(takeProfit->marketPosition);
  EXPECT_EQ(takeProfit->distanceToBestTicks, plain->distanceToBestTicks);
}

// The same defect stated without the comparison: 5 lots rest at 101.0 on
// the ask, so no order resting there is alone at its level, whatever its
// type. LevelEmpty is the shape the missing queue-tracker registration
// takes -- the order's own level looks empty because the tracker never
// took it on.
TEST(MarketPosition, FiredTakeProfitLimitIsNotAloneAtALevelWithDepth)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(takeProfitLimitSell(1, 1, 100.5, 101.0, 1.0));
  exec.onTrade(1, Price::fromDouble(100.6), Quantity::fromDouble(1.0), true);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  const OrderEvent* ev = findMarketPositionEvent(events, 1);
  ASSERT_NE(ev, nullptr) << "the fired take-profit limit reported no position";
  EXPECT_NE(ev->marketPosition, MarketPosition::LevelEmpty)
      << "reported as alone at 101.0 while 5 lots rest there";
  EXPECT_EQ(ev->marketPosition, MarketPosition::Best);
}
