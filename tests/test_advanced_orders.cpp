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
#include "flox/book/book_update.h"
#include "flox/execution/events/order_event.h"
#include "flox/strategy/signal.h"

#include <gtest/gtest.h>
#include <vector>

using namespace flox;

class AdvancedOrdersTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    currentTimeNs = 1000000000;  // 1 second
    clock = std::make_unique<SimulatedClock>(currentTimeNs);
    executor = std::make_unique<SimulatedExecutor>(*clock);
    executor->setOrderEventCallback([this](const OrderEvent& ev)
                                    { events.push_back(ev); });
  }

  void advanceTime(UnixNanos deltaNs)
  {
    currentTimeNs += deltaNs;
    clock->advanceTo(currentTimeNs);
  }

  void sendTrade(SymbolId sym, Price price)
  {
    executor->onTrade(sym, price, true);
    advanceTime(1000000);  // 1ms
  }

  void sendBook(SymbolId sym, Price bid, Price ask)
  {
    std::pmr::vector<BookLevel> bids{{bid, Quantity::fromDouble(1.0)}};
    std::pmr::vector<BookLevel> asks{{ask, Quantity::fromDouble(1.0)}};
    executor->onBookUpdate(sym, bids, asks);
    advanceTime(1000000);
  }

  UnixNanos currentTimeNs{0};
  std::unique_ptr<SimulatedClock> clock;
  std::unique_ptr<SimulatedExecutor> executor;
  std::vector<OrderEvent> events;
};

// ============================================================================
// Stop Market Tests
// ============================================================================

TEST_F(AdvancedOrdersTest, StopMarketSellTriggers)
{
  // Setup: price at 100
  sendTrade(1, Price::fromDouble(100.0));

  // Place stop market sell at 95
  Order order;
  order.id = 1;
  order.symbol = 1;
  order.side = Side::SELL;
  order.type = OrderType::STOP_MARKET;
  order.triggerPrice = Price::fromDouble(95.0);
  order.quantity = Quantity::fromDouble(1.0);

  executor->submitOrder(order);

  // Find PENDING_TRIGGER event
  bool foundPending = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::PENDING_TRIGGER)
    {
      foundPending = true;
      break;
    }
  }
  EXPECT_TRUE(foundPending);

  // Price drops below trigger
  sendTrade(1, Price::fromDouble(94.0));

  // Should be triggered and filled
  bool foundTriggered = false;
  bool foundFilled = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::TRIGGERED)
    {
      foundTriggered = true;
    }
    if (ev.status == OrderEventStatus::FILLED)
    {
      foundFilled = true;
    }
  }
  EXPECT_TRUE(foundTriggered);
  EXPECT_TRUE(foundFilled);
}

TEST_F(AdvancedOrdersTest, StopMarketBuyTriggers)
{
  // Setup: price at 100
  sendTrade(1, Price::fromDouble(100.0));

  // Place stop market buy at 105
  Order order;
  order.id = 1;
  order.symbol = 1;
  order.side = Side::BUY;
  order.type = OrderType::STOP_MARKET;
  order.triggerPrice = Price::fromDouble(105.0);
  order.quantity = Quantity::fromDouble(1.0);

  executor->submitOrder(order);

  // Price rises above trigger
  sendBook(1, Price::fromDouble(104.0), Price::fromDouble(106.0));
  sendTrade(1, Price::fromDouble(106.0));

  bool foundFilled = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED)
    {
      foundFilled = true;
    }
  }
  EXPECT_TRUE(foundFilled);
}

// ============================================================================
// Take Profit Tests
// ============================================================================

TEST_F(AdvancedOrdersTest, TakeProfitSellTriggers)
{
  // Setup: price at 100
  sendTrade(1, Price::fromDouble(100.0));

  // Place TP sell at 110 (for long position)
  Order order;
  order.id = 1;
  order.symbol = 1;
  order.side = Side::SELL;
  order.type = OrderType::TAKE_PROFIT_MARKET;
  order.triggerPrice = Price::fromDouble(110.0);
  order.quantity = Quantity::fromDouble(1.0);

  executor->submitOrder(order);

  // Price rises above trigger
  sendBook(1, Price::fromDouble(109.0), Price::fromDouble(111.0));
  sendTrade(1, Price::fromDouble(111.0));

  bool foundFilled = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED)
    {
      foundFilled = true;
    }
  }
  EXPECT_TRUE(foundFilled);
}

// ============================================================================
// Trailing Stop Tests
// ============================================================================

TEST_F(AdvancedOrdersTest, TrailingStopFollowsPrice)
{
  // Setup: price at 100
  sendTrade(1, Price::fromDouble(100.0));
  sendBook(1, Price::fromDouble(99.0), Price::fromDouble(101.0));

  // Place trailing stop with 5 offset
  Order order;
  order.id = 1;
  order.symbol = 1;
  order.side = Side::SELL;
  order.type = OrderType::TRAILING_STOP;
  order.trailingOffset = Price::fromDouble(5.0);
  order.quantity = Quantity::fromDouble(1.0);

  executor->submitOrder(order);

  // Initial trigger should be at 95 (100 - 5)
  events.clear();

  // Price rises to 110, trigger should move to 105
  sendTrade(1, Price::fromDouble(110.0));

  bool foundUpdate = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::TRAILING_UPDATED)
    {
      foundUpdate = true;
      // New trigger should be 105 (110 - 5)
      EXPECT_EQ(ev.newTrailingPrice.raw(), Price::fromDouble(105.0).raw());
    }
  }
  EXPECT_TRUE(foundUpdate);

  events.clear();

  // Price drops to 104 - should trigger
  sendTrade(1, Price::fromDouble(104.0));

  bool foundTriggered = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::TRIGGERED)
    {
      foundTriggered = true;
    }
  }
  EXPECT_TRUE(foundTriggered);
}

TEST_F(AdvancedOrdersTest, TrailingStopPercent)
{
  // Setup: price at 100
  sendTrade(1, Price::fromDouble(100.0));
  sendBook(1, Price::fromDouble(99.0), Price::fromDouble(101.0));

  // Place trailing stop with 5% callback (500 bps)
  Order order;
  order.id = 1;
  order.symbol = 1;
  order.side = Side::SELL;
  order.type = OrderType::TRAILING_STOP;
  order.trailingCallbackRate = 500;  // 5%
  order.quantity = Quantity::fromDouble(1.0);

  executor->submitOrder(order);

  // Initial trigger should be at 95 (100 - 5%)
  events.clear();

  // Price rises to 200, trigger should move to 190 (200 - 5%)
  sendTrade(1, Price::fromDouble(200.0));

  bool foundUpdate = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::TRAILING_UPDATED)
    {
      foundUpdate = true;
      EXPECT_EQ(ev.newTrailingPrice.raw(), Price::fromDouble(190.0).raw());
    }
  }
  EXPECT_TRUE(foundUpdate);
}

// ============================================================================
// OCO Tests
// ============================================================================

TEST_F(AdvancedOrdersTest, OCOCancelsOther)
{
  // Setup
  sendBook(1, Price::fromDouble(99.0), Price::fromDouble(101.0));

  Order order1;
  order1.id = 1;
  order1.symbol = 1;
  order1.side = Side::BUY;
  order1.type = OrderType::LIMIT;
  order1.price = Price::fromDouble(95.0);
  order1.quantity = Quantity::fromDouble(1.0);

  Order order2;
  order2.id = 2;
  order2.symbol = 1;
  order2.side = Side::BUY;
  order2.type = OrderType::LIMIT;
  order2.price = Price::fromDouble(98.0);
  order2.quantity = Quantity::fromDouble(1.0);

  OCOParams params;
  params.order1 = order1;
  params.order2 = order2;

  executor->submitOCO(params);

  events.clear();

  // Fill order2 by moving price
  sendBook(1, Price::fromDouble(97.0), Price::fromDouble(98.0));

  // order2 should fill
  bool order2Filled = false;
  bool order1Canceled = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED && ev.order.id == 2)
    {
      order2Filled = true;
    }
    if (ev.status == OrderEventStatus::CANCELED && ev.order.id == 1)
    {
      order1Canceled = true;
    }
  }

  EXPECT_TRUE(order2Filled);
  EXPECT_TRUE(order1Canceled);
}

// ============================================================================
// Signal Factory Tests
// ============================================================================

TEST_F(AdvancedOrdersTest, SignalFactories)
{
  // Test all signal factories compile and create correct types
  auto stopMarket = Signal::stopMarket(1, Side::SELL, Price::fromDouble(95.0),
                                       Quantity::fromDouble(1.0), 100);
  EXPECT_EQ(stopMarket.type, SignalType::StopMarket);
  EXPECT_EQ(stopMarket.triggerPrice.raw(), Price::fromDouble(95.0).raw());

  auto stopLimit = Signal::stopLimit(1, Side::SELL, Price::fromDouble(95.0),
                                     Price::fromDouble(94.0), Quantity::fromDouble(1.0), 101);
  EXPECT_EQ(stopLimit.type, SignalType::StopLimit);

  auto tpMarket = Signal::takeProfitMarket(1, Side::SELL, Price::fromDouble(110.0),
                                           Quantity::fromDouble(1.0), 102);
  EXPECT_EQ(tpMarket.type, SignalType::TakeProfitMarket);

  auto tpLimit = Signal::takeProfitLimit(1, Side::SELL, Price::fromDouble(110.0),
                                         Price::fromDouble(109.0), Quantity::fromDouble(1.0), 103);
  EXPECT_EQ(tpLimit.type, SignalType::TakeProfitLimit);

  auto trailing = Signal::trailingStop(1, Side::SELL, Price::fromDouble(5.0),
                                       Quantity::fromDouble(1.0), 104);
  EXPECT_EQ(trailing.type, SignalType::TrailingStop);

  auto trailingPct = Signal::trailingStopPercent(1, Side::SELL, 500, Quantity::fromDouble(1.0), 105);
  EXPECT_EQ(trailingPct.type, SignalType::TrailingStop);
  EXPECT_EQ(trailingPct.trailingCallbackRate, 500);

  auto oco = Signal::oco(1, Side::BUY, Price::fromDouble(95.0), Price::fromDouble(105.0),
                         Quantity::fromDouble(1.0), 106);
  EXPECT_EQ(oco.type, SignalType::OCO);
  EXPECT_EQ(oco.price.raw(), Price::fromDouble(95.0).raw());
  EXPECT_EQ(oco.triggerPrice.raw(), Price::fromDouble(105.0).raw());
}

// ============================================================================
// TimeInForce and Flags Tests
// ============================================================================

TEST_F(AdvancedOrdersTest, SignalModifiers)
{
  auto signal = Signal::limitBuy(1, Price::fromDouble(100.0), Quantity::fromDouble(1.0), 1)
                    .withTimeInForce(TimeInForce::IOC)
                    .withReduceOnly()
                    .withPostOnly();

  EXPECT_EQ(signal.timeInForce, TimeInForce::IOC);
  EXPECT_TRUE(signal.reduceOnly);
  EXPECT_TRUE(signal.postOnly);
}

// ============================================================================
// ExchangeCapabilities Tests
// ============================================================================

TEST_F(AdvancedOrdersTest, ExchangeCapabilities)
{
  auto caps = executor->capabilities();

  EXPECT_TRUE(caps.supports(OrderType::LIMIT));
  EXPECT_TRUE(caps.supports(OrderType::MARKET));
  EXPECT_TRUE(caps.supports(OrderType::STOP_MARKET));
  EXPECT_TRUE(caps.supports(OrderType::TRAILING_STOP));

  EXPECT_TRUE(caps.supports(TimeInForce::GTC));
  EXPECT_TRUE(caps.supports(TimeInForce::IOC));
}
