/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/aggregator/bus/candle_bus.h"
#include "flox/aggregator/candle_aggregator.h"
#include "flox/aggregator/events/candle_event.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/engine/market_data_subscriber_component.h"
#include "flox/strategy/strategy_component.h"
#include "flox/util/base/ref.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <vector>

#ifndef USE_SYNC_CANDLE_BUS
#error "Test requires USE_SYNC_CANDLE_BUS to be defined"
#endif

using namespace flox;

namespace
{

constexpr SymbolId SYMBOL = 42;
const std::chrono::seconds INTERVAL = std::chrono::seconds(60);

std::chrono::steady_clock::time_point ts(int seconds)
{
  return std::chrono::steady_clock::time_point(std::chrono::seconds(seconds));
}

TradeEvent makeTrade(SymbolId symbol, double price, double qty, int sec)
{
  TradeEvent event;
  event.trade.symbol = symbol;
  event.trade.price = Price::fromDouble(price);
  event.trade.quantity = Quantity::fromDouble(qty);
  event.trade.isBuy = true;
  event.trade.timestamp = ts(sec);
  return event;
}

class TestStrategy
{
 public:
  using Trait = traits::StrategyTrait;
  using Allocator = PoolAllocator<Trait, 8>;

  explicit TestStrategy(std::vector<Candle>& out, std::vector<SymbolId>* symOut = nullptr)
      : _out(out), _symOut(symOut) {}

  void start() {}
  void stop() {}

  SubscriberId id() const { return reinterpret_cast<SubscriberId>(this); }
  SubscriberMode mode() const { return SubscriberMode::PUSH; }

  void onCandle(const CandleEvent& event)
  {
    _out.push_back(event.candle);
    if (_symOut)
      _symOut->push_back(event.symbol);
  }
  void onBookUpdate(const BookUpdateEvent&) {}
  void onTrade(const TradeEvent&) {}

 private:
  std::vector<Candle>& _out;
  std::vector<SymbolId>* _symOut;
};
static_assert(concepts::Strategy<TestStrategy>);

}  // namespace

TEST(CandleAggregatorTest, AllEventsAreDeliveredBeforeStop)
{
  std::vector<Candle> result;
  auto strat = make<TestStrategy>(result);
  auto bus = make<CandleBus>();
  auto aggregator = make<CandleAggregator>(INTERVAL, bus);

  bus.subscribe(strat.as<traits::MarketDataSubscriberTrait>());

  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));   // open
  aggregator.onTrade(makeTrade(SYMBOL, 110, 1, 10));  // high
  aggregator.onTrade(makeTrade(SYMBOL, 95, 1, 20));   // low
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 50));  // close
  aggregator.onTrade(makeTrade(SYMBOL, 115, 1, 65));  // triggers flush of first, starts second

  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 2);

  const auto& first = result[0];
  EXPECT_EQ(first.startTime, ts(0));
  EXPECT_EQ(first.endTime, ts(60));
  EXPECT_EQ(first.open, Price::fromDouble(100));
  EXPECT_EQ(first.high, Price::fromDouble(110));
  EXPECT_EQ(first.low, Price::fromDouble(95));
  EXPECT_EQ(first.close, Price::fromDouble(105));

  const auto& second = result[1];
  EXPECT_EQ(second.startTime, ts(60));
  EXPECT_EQ(second.endTime, ts(120));
  EXPECT_EQ(second.open, Price::fromDouble(115));
  EXPECT_EQ(second.high, Price::fromDouble(115));
  EXPECT_EQ(second.low, Price::fromDouble(115));
  EXPECT_EQ(second.close, Price::fromDouble(115));
}

TEST(CandleAggregatorTest, NoEventsAreLostAcrossMultipleStarts)
{
  std::vector<Candle> result;
  auto strat = make<TestStrategy>(result);
  auto bus = make<CandleBus>();
  auto aggregator = make<CandleAggregator>(INTERVAL, bus);

  bus.subscribe(strat.as<traits::MarketDataSubscriberTrait>());

  bus.start();

  aggregator.start();
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.stop();  // first candle

  aggregator.start();
  aggregator.onTrade(makeTrade(SYMBOL, 120, 2, 70));
  aggregator.stop();  // second candle0

  bus.stop();

  ASSERT_EQ(result.size(), 2);
  EXPECT_EQ(result[0].open, Price::fromDouble(100));
  EXPECT_EQ(result[1].open, Price::fromDouble(120));
}

TEST(CandleAggregatorTest, MultipleSymbolsAreDeliveredIndependently)
{
  std::vector<Candle> candles;
  std::vector<SymbolId> symbols;
  auto strat = make<TestStrategy>(candles, &symbols);
  auto bus = make<CandleBus>();
  auto aggregator = make<CandleAggregator>(INTERVAL, bus);

  bus.subscribe(strat.as<traits::MarketDataSubscriberTrait>());

  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(1, 10, 1, 0));
  aggregator.onTrade(makeTrade(2, 20, 1, 0));
  aggregator.stop();
  bus.stop();

  ASSERT_EQ(candles.size(), 2);
  EXPECT_TRUE(std::count(symbols.begin(), symbols.end(), 1));
  EXPECT_TRUE(std::count(symbols.begin(), symbols.end(), 2));
}

TEST(CandleAggregatorTest, CandleIsGeneratedEvenFromSingleTrade)
{
  std::vector<Candle> result;
  auto strat = make<TestStrategy>(result);
  auto bus = make<CandleBus>();
  auto aggregator = make<CandleAggregator>(INTERVAL, bus);

  bus.subscribe(strat.as<traits::MarketDataSubscriberTrait>());

  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 111, 1, 0));

  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 1);
  const auto& c = result[0];
  EXPECT_EQ(c.open, Price::fromDouble(111));
  EXPECT_EQ(c.close, Price::fromDouble(111));
  EXPECT_EQ(c.volume, Volume::fromDouble(111));
}

TEST(CandleAggregatorTest, StopFlushesAllPendingCandles)
{
  std::vector<Candle> result;
  auto strat = make<TestStrategy>(result);
  auto bus = make<CandleBus>();
  auto aggregator = make<CandleAggregator>(INTERVAL, bus);

  bus.subscribe(strat.as<traits::MarketDataSubscriberTrait>());

  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 90, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 91, 1, 30));
  aggregator.onTrade(makeTrade(SYMBOL, 92, 1, 90));  // starts new

  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 2);
  EXPECT_EQ(result[0].close, Price::fromDouble(91));
  EXPECT_EQ(result[1].close, Price::fromDouble(92));
}
