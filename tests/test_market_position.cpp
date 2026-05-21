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
