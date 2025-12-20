/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/position_tracker.h"
#include "flox/strategy/strategy.h"
#include "flox/strategy/symbol_context.h"
#include "flox/strategy/symbol_state_map.h"

#include <gtest/gtest.h>
#include <memory_resource>

using namespace flox;

namespace
{
void populateRegistry(SymbolRegistry& registry, const std::vector<SymbolId>& symbols)
{
  for (SymbolId sym : symbols)
  {
    SymbolInfo info;
    info.exchange = "TEST";
    info.symbol = "SYM" + std::to_string(sym);
    info.tickSize = Price::fromDouble(0.01);
    registry.registerSymbol(info);
  }
}
}  // namespace

class SymbolStateMapTest : public ::testing::Test
{
};

TEST_F(SymbolStateMapTest, FlatArrayAccess)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState> map;

  map[5].value = 42;
  EXPECT_EQ(map[5].value, 42);
  EXPECT_TRUE(map.contains(5));
  EXPECT_FALSE(map.contains(6));
}

TEST_F(SymbolStateMapTest, OverflowAccess)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState, 10> map;

  map[500].value = 99;
  EXPECT_EQ(map[500].value, 99);
  EXPECT_TRUE(map.contains(500));
}

TEST_F(SymbolStateMapTest, TryGetReturnsNullptrForMissing)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState> map;

  EXPECT_EQ(map.tryGet(5), nullptr);
  map[5].value = 10;
  EXPECT_NE(map.tryGet(5), nullptr);
  EXPECT_EQ(map.tryGet(5)->value, 10);
}

TEST_F(SymbolStateMapTest, ForEachIteratesAll)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState, 10> map;

  map[1].value = 1;
  map[2].value = 2;
  map[300].value = 300;

  int sum = 0;
  map.forEach([&sum](SymbolId, const TestState& s)
              { sum += s.value; });
  EXPECT_EQ(sum, 303);
}

TEST_F(SymbolStateMapTest, ClearResetsAll)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState> map;

  map[1].value = 100;
  map[2].value = 200;
  EXPECT_EQ(map.size(), 2);

  map.clear();
  EXPECT_EQ(map.size(), 0);
  EXPECT_FALSE(map.contains(1));
}

class SymbolContextTest : public ::testing::Test
{
 protected:
  std::pmr::monotonic_buffer_resource _resource{4096};
};

TEST_F(SymbolContextTest, MidPriceCalculation)
{
  SymbolContext ctx(Price::fromDouble(0.01));

  BookUpdateEvent ev(&_resource);
  ev.update.type = BookUpdateType::SNAPSHOT;
  ev.update.bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(1.0));
  ev.update.asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(1.0));

  ctx.book.applyBookUpdate(ev);

  auto mid = ctx.mid();
  ASSERT_TRUE(mid.has_value());
  EXPECT_NEAR(mid->toDouble(), 100.5, 0.01);
}

TEST_F(SymbolContextTest, UnrealizedPnlLong)
{
  SymbolContext ctx(Price::fromDouble(0.01));
  ctx.position = Quantity::fromDouble(10.0);
  ctx.avgEntryPrice = Price::fromDouble(100.0);

  double pnl = ctx.unrealizedPnl(Price::fromDouble(110.0));
  EXPECT_NEAR(pnl, 100.0, 0.01);
}

TEST_F(SymbolContextTest, UnrealizedPnlShort)
{
  SymbolContext ctx(Price::fromDouble(0.01));
  ctx.position = Quantity::fromDouble(-10.0);
  ctx.avgEntryPrice = Price::fromDouble(100.0);

  double pnl = ctx.unrealizedPnl(Price::fromDouble(90.0));
  EXPECT_NEAR(pnl, 100.0, 0.01);
}

TEST_F(SymbolContextTest, PositionFlags)
{
  SymbolContext ctx(Price::fromDouble(0.01));

  EXPECT_TRUE(ctx.isFlat());
  EXPECT_FALSE(ctx.isLong());
  EXPECT_FALSE(ctx.isShort());

  ctx.position = Quantity::fromDouble(1.0);
  EXPECT_TRUE(ctx.isLong());
  EXPECT_FALSE(ctx.isFlat());

  ctx.position = Quantity::fromDouble(-1.0);
  EXPECT_TRUE(ctx.isShort());
}

class TestStrategy : public Strategy
{
 public:
  TestStrategy(std::vector<SymbolId> syms, const SymbolRegistry& registry)
      : Strategy(1, std::move(syms), registry)
  {
  }

  void start() override {}
  void stop() override {}

  int tradeCount{0};
  int bookCount{0};

  using Strategy::ctx;

 protected:
  void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override { ++tradeCount; }

  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override { ++bookCount; }
};

class MultiSymbolStrategyTest : public ::testing::Test
{
 protected:
  std::pmr::monotonic_buffer_resource _resource{4096};
};

TEST_F(MultiSymbolStrategyTest, FiltersUnsubscribedSymbols)
{
  SymbolRegistry registry;
  populateRegistry(registry, {1, 2});
  TestStrategy strategy({1, 2}, registry);

  TradeEvent ev1, ev2, ev3;
  ev1.trade.symbol = 1;
  ev2.trade.symbol = 2;
  ev3.trade.symbol = 3;

  strategy.onTrade(ev1);
  strategy.onTrade(ev2);
  strategy.onTrade(ev3);

  EXPECT_EQ(strategy.tradeCount, 2);
}

TEST_F(MultiSymbolStrategyTest, ContextAccessible)
{
  SymbolRegistry registry;
  populateRegistry(registry, {1, 2});
  TestStrategy strategy({1, 2}, registry);

  TradeEvent ev;
  ev.trade.symbol = 1;
  ev.trade.price = Price::fromDouble(100.0);

  strategy.onTrade(ev);

  EXPECT_EQ(strategy.ctx(1).lastTradePrice.toDouble(), 100.0);
}

TEST_F(MultiSymbolStrategyTest, SpreadCalculation)
{
  SymbolRegistry registry;
  populateRegistry(registry, {1, 2});
  TestStrategy strategy({1, 2}, registry);

  BookUpdateEvent ev1(&_resource);
  ev1.update.symbol = 1;
  ev1.update.type = BookUpdateType::SNAPSHOT;
  ev1.update.bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(1.0));
  ev1.update.asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(1.0));

  BookUpdateEvent ev2(&_resource);
  ev2.update.symbol = 2;
  ev2.update.type = BookUpdateType::SNAPSHOT;
  ev2.update.bids.emplace_back(Price::fromDouble(50.0), Quantity::fromDouble(1.0));
  ev2.update.asks.emplace_back(Price::fromDouble(51.0), Quantity::fromDouble(1.0));

  strategy.onBookUpdate(ev1);
  strategy.onBookUpdate(ev2);

  auto spreadOpt = spread(strategy.ctx(1), strategy.ctx(2));
  ASSERT_TRUE(spreadOpt.has_value());
  EXPECT_NEAR(spreadOpt->toDouble(), 50.0, 0.5);
}

class PositionTrackerTest : public ::testing::Test
{
};

TEST_F(PositionTrackerTest, TracksBuyPosition)
{
  PositionTracker tracker(1);

  Order order{.id = 1,
              .side = Side::BUY,
              .price = Price::fromDouble(100.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::MARKET,
              .symbol = 1};

  tracker.onOrderFilled(order);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, TracksSellPosition)
{
  PositionTracker tracker(1);

  Order order{.id = 1,
              .side = Side::SELL,
              .price = Price::fromDouble(100.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::MARKET,
              .symbol = 1};

  tracker.onOrderFilled(order);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), -10.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, CalculatesRealizedPnlOnClose)
{
  PositionTracker tracker(1);

  Order buy{.id = 1,
            .side = Side::BUY,
            .price = Price::fromDouble(100.0),
            .quantity = Quantity::fromDouble(10.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  tracker.onOrderFilled(buy);

  Order sell{.id = 2,
             .side = Side::SELL,
             .price = Price::fromDouble(110.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 0.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 100.0, 0.01);
}

TEST_F(PositionTrackerTest, VWAPOnAddingToPosition)
{
  PositionTracker tracker(1);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(110.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy2);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 20.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 105.0);
}

TEST_F(PositionTrackerTest, PartialFillUpdatesPosition)
{
  PositionTracker tracker(1);

  Order order{.id = 1,
              .side = Side::BUY,
              .price = Price::fromDouble(100.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::LIMIT,
              .symbol = 1};

  tracker.onOrderPartiallyFilled(order, Quantity::fromDouble(5.0));

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 5.0);
}

TEST_F(PositionTrackerTest, MultipleSymbolsTrackedSeparately)
{
  PositionTracker tracker(1);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(50.0),
             .quantity = Quantity::fromDouble(20.0),
             .type = OrderType::MARKET,
             .symbol = 2};
  tracker.onOrderFilled(buy2);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_EQ(tracker.getPosition(2).toDouble(), 20.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(2).toDouble(), 50.0);
}

TEST_F(PositionTrackerTest, FIFOPnlCalculation)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(120.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy2);

  Order sell{.id = 3,
             .side = Side::SELL,
             .price = Price::fromDouble(115.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 150.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 120.0);
}

TEST_F(PositionTrackerTest, LIFOPnlCalculation)
{
  PositionTracker tracker(1, CostBasisMethod::LIFO);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(120.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy2);

  Order sell{.id = 3,
             .side = Side::SELL,
             .price = Price::fromDouble(115.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), -50.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, AveragePnlCalculation)
{
  PositionTracker tracker(1, CostBasisMethod::AVERAGE);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(120.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy2);

  Order sell{.id = 3,
             .side = Side::SELL,
             .price = Price::fromDouble(115.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 50.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 110.0);
}

TEST_F(PositionTrackerTest, ShortPositionFIFO)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  Order sell1{.id = 1,
              .side = Side::SELL,
              .price = Price::fromDouble(120.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::MARKET,
              .symbol = 1};
  tracker.onOrderFilled(sell1);

  Order sell2{.id = 2,
              .side = Side::SELL,
              .price = Price::fromDouble(100.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::MARKET,
              .symbol = 1};
  tracker.onOrderFilled(sell2);

  Order buy{.id = 3,
            .side = Side::BUY,
            .price = Price::fromDouble(105.0),
            .quantity = Quantity::fromDouble(10.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  tracker.onOrderFilled(buy);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), -10.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 150.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, PartialLotClose)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  Order buy{.id = 1,
            .side = Side::BUY,
            .price = Price::fromDouble(100.0),
            .quantity = Quantity::fromDouble(20.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  tracker.onOrderFilled(buy);

  Order sell{.id = 2,
             .side = Side::SELL,
             .price = Price::fromDouble(110.0),
             .quantity = Quantity::fromDouble(5.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 15.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 50.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, FlipPositionLongToShort)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  Order buy{.id = 1,
            .side = Side::BUY,
            .price = Price::fromDouble(100.0),
            .quantity = Quantity::fromDouble(10.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  tracker.onOrderFilled(buy);

  Order sell{.id = 2,
             .side = Side::SELL,
             .price = Price::fromDouble(110.0),
             .quantity = Quantity::fromDouble(15.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), -5.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 100.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 110.0);
}

// Tests for Strategy integration with OrderTracker and PositionManager

class SignalCapture : public ISignalHandler
{
 public:
  void onSignal(const Signal& signal) override { signals.push_back(signal); }
  std::vector<Signal> signals;
};

class StrategyIntegrationTest : public ::testing::Test
{
 protected:
  SymbolRegistry registry;
  SignalCapture signalCapture;
  OrderTracker orderTracker;
  PositionTracker positionTracker{1};

  void SetUp() override { populateRegistry(registry, {1, 2}); }
};

TEST_F(StrategyIntegrationTest, EmitMarketBuyReturnsOrderId)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::emitMarketBuy;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy testable({1}, registry);
  testable.setSignalHandler(&signalCapture);

  OrderId id1 = testable.emitMarketBuy(1, Quantity::fromDouble(10.0));
  OrderId id2 = testable.emitMarketBuy(1, Quantity::fromDouble(5.0));

  EXPECT_NE(id1, id2);
  EXPECT_EQ(signalCapture.signals.size(), 2);
  EXPECT_EQ(signalCapture.signals[0].orderId, id1);
  EXPECT_EQ(signalCapture.signals[1].orderId, id2);
}

TEST_F(StrategyIntegrationTest, EmitModifyGeneratesModifySignal)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::emitLimitBuy;
    using Strategy::emitModify;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1}, registry);
  strategy.setSignalHandler(&signalCapture);

  OrderId id = strategy.emitLimitBuy(1, Price::fromDouble(100.0), Quantity::fromDouble(10.0));
  strategy.emitModify(id, Price::fromDouble(99.0), Quantity::fromDouble(5.0));

  EXPECT_EQ(signalCapture.signals.size(), 2);
  EXPECT_EQ(signalCapture.signals[1].type, SignalType::Modify);
  EXPECT_EQ(signalCapture.signals[1].orderId, id);
  EXPECT_EQ(signalCapture.signals[1].newPrice.toDouble(), 99.0);
  EXPECT_EQ(signalCapture.signals[1].newQuantity.toDouble(), 5.0);
}

TEST_F(StrategyIntegrationTest, GetOrderStatusFromTracker)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::getOrder;
    using Strategy::getOrderStatus;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1}, registry);
  strategy.setOrderTracker(&orderTracker);

  Order order{.id = 42, .side = Side::BUY, .price = Price::fromDouble(100.0), .quantity = Quantity::fromDouble(10.0)};
  orderTracker.onSubmitted(order, "exch-123");

  auto status = strategy.getOrderStatus(42);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, OrderEventStatus::SUBMITTED);

  auto orderState = strategy.getOrder(42);
  ASSERT_TRUE(orderState.has_value());
  EXPECT_EQ(orderState->exchangeOrderId, "exch-123");
}

TEST_F(StrategyIntegrationTest, GetOrderStatusWithoutTrackerReturnsNullopt)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::getOrderStatus;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1}, registry);
  // No tracker set

  auto status = strategy.getOrderStatus(42);
  EXPECT_FALSE(status.has_value());
}

TEST_F(StrategyIntegrationTest, PositionFromPositionManager)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::position;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1, 2}, registry);
  strategy.setPositionManager(&positionTracker);

  Order buy{.id = 1,
            .side = Side::BUY,
            .price = Price::fromDouble(100.0),
            .quantity = Quantity::fromDouble(10.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  positionTracker.onOrderFilled(buy);

  EXPECT_EQ(strategy.position(1).toDouble(), 10.0);
  EXPECT_EQ(strategy.position(2).toDouble(), 0.0);
  EXPECT_EQ(strategy.position().toDouble(), 10.0);  // First symbol
}

TEST_F(StrategyIntegrationTest, PositionWithoutManagerReturnsZero)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::position;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1}, registry);
  // No position manager set

  EXPECT_EQ(strategy.position(1).toDouble(), 0.0);
}

TEST_F(StrategyIntegrationTest, GlobalOrderIdIsUnique)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::emitMarketBuy;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy1({1}, registry);
  TestableStrategy strategy2({2}, registry);
  strategy1.setSignalHandler(&signalCapture);
  strategy2.setSignalHandler(&signalCapture);

  OrderId id1 = strategy1.emitMarketBuy(1, Quantity::fromDouble(1.0));
  OrderId id2 = strategy2.emitMarketBuy(2, Quantity::fromDouble(1.0));
  OrderId id3 = strategy1.emitMarketBuy(1, Quantity::fromDouble(1.0));

  EXPECT_NE(id1, id2);
  EXPECT_NE(id2, id3);
  EXPECT_NE(id1, id3);
}
