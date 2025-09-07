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
#include "flox/book/events/trade_event.h"
#include "flox/common.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <vector>

using namespace flox;

namespace
{

constexpr SymbolId SYMBOL = 42;
const std::chrono::seconds INTERVAL = std::chrono::seconds(60);

TimePoint ts(int seconds) { return TimePoint(std::chrono::seconds(seconds)); }

TradeEvent makeTrade(SymbolId symbol, double price, double qty, int sec)
{
  TradeEvent event;
  event.trade.symbol = symbol;
  event.trade.price = Price::fromDouble(price);
  event.trade.quantity = Quantity::fromDouble(qty);
  event.trade.isBuy = true;
  event.trade.exchangeTsNs = ts(sec).time_since_epoch().count();
  return event;
}

class TestStrategy : public CandleEvent::Listener
{
 public:
  explicit TestStrategy(std::vector<Candle>& out, std::vector<SymbolId>* symOut = nullptr)
      : _out(out), _symOut(symOut) {}

  SubscriberId id() const override { return reinterpret_cast<SubscriberId>(this); }

  void onCandle(const CandleEvent& event) override
  {
    _out.push_back(event.candle);
    if (_symOut)
    {
      _symOut->push_back(event.symbol);
    }
  }

 private:
  std::vector<Candle>& _out;
  std::vector<SymbolId>* _symOut;
};

}  // namespace

TEST(CandleAggregatorTest, AllEventsAreDeliveredBeforeStop)
{
  std::vector<Candle> result;
  CandleBus bus;
  CandleAggregator aggregator(INTERVAL, &bus);
  auto strat = std::make_unique<TestStrategy>(result);

  bus.subscribe(strat.get(), /*required=*/true);
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));   // open
  aggregator.onTrade(makeTrade(SYMBOL, 110, 1, 10));  // high
  aggregator.onTrade(makeTrade(SYMBOL, 95, 1, 20));   // low
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 50));  // close
  aggregator.onTrade(makeTrade(SYMBOL, 115, 1, 65));  // flush first, start second

  aggregator.stop();
  bus.flush();
  bus.stop();

  ASSERT_EQ(result.size(), 2u);

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
  CandleBus bus;
  CandleAggregator aggregator(INTERVAL, &bus);
  auto strat = std::make_unique<TestStrategy>(result);

  bus.subscribe(strat.get());
  bus.start();

  aggregator.start();
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.stop();
  bus.flush();

  aggregator.start();
  aggregator.onTrade(makeTrade(SYMBOL, 120, 2, 70));
  aggregator.stop();
  bus.flush();

  bus.stop();

  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].open, Price::fromDouble(100));
  EXPECT_EQ(result[1].open, Price::fromDouble(120));
}

TEST(CandleAggregatorTest, MultipleSymbolsAreDeliveredIndependently)
{
  std::vector<Candle> candles;
  std::vector<SymbolId> symbols;
  CandleBus bus;
  CandleAggregator aggregator(INTERVAL, &bus);
  auto strat = std::make_unique<TestStrategy>(candles, &symbols);

  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(1, 10, 1, 0));
  aggregator.onTrade(makeTrade(2, 20, 1, 0));

  aggregator.stop();
  bus.flush();
  bus.stop();

  ASSERT_EQ(candles.size(), 2u);
  EXPECT_TRUE(std::count(symbols.begin(), symbols.end(), 1));
  EXPECT_TRUE(std::count(symbols.begin(), symbols.end(), 2));
}

TEST(CandleAggregatorTest, CandleIsGeneratedEvenFromSingleTrade)
{
  std::vector<Candle> result;
  CandleBus bus;
  CandleAggregator aggregator(INTERVAL, &bus);
  auto strat = std::make_unique<TestStrategy>(result);

  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 111, 1, 0));
  aggregator.stop();
  bus.flush();
  bus.stop();

  ASSERT_EQ(result.size(), 1u);
  const auto& c = result[0];
  EXPECT_EQ(c.open, Price::fromDouble(111));
  EXPECT_EQ(c.close, Price::fromDouble(111));
  EXPECT_EQ(c.volume, Volume::fromDouble(111));
}

TEST(CandleAggregatorTest, StopFlushesAllPendingCandles)
{
  std::vector<Candle> result;
  CandleBus bus;
  CandleAggregator aggregator(INTERVAL, &bus);
  auto strat = std::make_unique<TestStrategy>(result);

  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 90, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 91, 1, 30));
  aggregator.onTrade(makeTrade(SYMBOL, 92, 1, 90));  // starts new

  aggregator.stop();
  bus.flush();
  bus.stop();

  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].close, Price::fromDouble(91));
  EXPECT_EQ(result[1].close, Price::fromDouble(92));
}
