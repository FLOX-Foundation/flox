/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/aggregator/bar_aggregator.h"
#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/custom/footprint_bar.h"
#include "flox/aggregator/custom/market_profile.h"
#include "flox/aggregator/custom/volume_profile.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/aggregator/multi_timeframe_aggregator.h"
#include "flox/aggregator/policies/range_bar_policy.h"
#include "flox/aggregator/policies/renko_bar_policy.h"
#include "flox/aggregator/policies/tick_bar_policy.h"
#include "flox/aggregator/policies/time_bar_policy.h"
#include "flox/aggregator/policies/volume_bar_policy.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/engine/symbol_registry.h"
#include "flox/strategy/abstract_strategy.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{

constexpr SymbolId SYMBOL = 42;
const std::chrono::seconds INTERVAL = std::chrono::seconds(60);

TimePoint ts(int seconds) { return TimePoint(std::chrono::seconds(seconds)); }

TradeEvent makeTrade(SymbolId symbol, double price, double qty, int sec,
                     InstrumentType instrument = InstrumentType::Spot, bool isBuy = true)
{
  TradeEvent event;
  event.trade.symbol = symbol;
  event.trade.instrument = instrument;
  event.trade.price = Price::fromDouble(price);
  event.trade.quantity = Quantity::fromDouble(qty);
  event.trade.isBuy = isBuy;
  event.trade.exchangeTsNs = ts(sec).time_since_epoch().count();
  return event;
}

class TestStrategy : public IStrategy
{
 public:
  explicit TestStrategy(std::vector<Bar>& out, std::vector<SymbolId>* symOut = nullptr)
      : _out(out), _symOut(symOut)
  {
  }

  SubscriberId id() const override { return 1; }

  void onBar(const BarEvent& event) override
  {
    _out.push_back(event.bar);
    if (_symOut)
    {
      _symOut->push_back(event.symbol);
    }
  }

 private:
  std::vector<Bar>& _out;
  std::vector<SymbolId>* _symOut;
};

}  // namespace

// ============================================================================
// TimeBarPolicy Tests
// ============================================================================

TEST(TimeBarPolicyTest, ClosesOnIntervalBoundary)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());

  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 2, 10));
  aggregator.onTrade(makeTrade(SYMBOL, 99, 3, 20));
  aggregator.onTrade(makeTrade(SYMBOL, 101, 1, 30));
  aggregator.onTrade(makeTrade(SYMBOL, 102, 2, 65));  // triggers flush

  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].high, Price::fromDouble(105.0));
  EXPECT_EQ(result[0].low, Price::fromDouble(99.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(101.0));
  EXPECT_EQ(result[0].volume, Volume::fromDouble(100 * 1 + 105 * 2 + 99 * 3 + 101 * 1));
  EXPECT_EQ(result[0].startTime, ts(0));
  EXPECT_EQ(result[0].endTime, ts(60));
}

TEST(TimeBarPolicyTest, HandlesGaps)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 110, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 120, 2, 130));  // gap → flush

  bus.stop();
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].startTime, ts(0));
  EXPECT_EQ(result[0].endTime, ts(60));
  EXPECT_EQ(result[0].close, Price::fromDouble(110.0));
}

TEST(TimeBarPolicyTest, AlignsToIntervalStart)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  // Trade at second 15 should align to interval starting at 0
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 15));
  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].startTime, ts(0));
  EXPECT_EQ(result[0].endTime, ts(60));
}

TEST(TimeBarPolicyTest, FlushesFinalBarOnStop)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 30));

  aggregator.stop();  // flush remaining
  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].high, Price::fromDouble(105.0));
  EXPECT_EQ(result[0].low, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(105.0));
  EXPECT_EQ(result[0].volume, Volume::fromDouble(100 * 1 + 105 * 1));
}

// ============================================================================
// TickBarPolicy Tests
// ============================================================================

TEST(TickBarPolicyTest, ClosesAfterNTrades)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  BarAggregator<TickBarPolicy> aggregator(TickBarPolicy(3), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 101, 1, 1));
  aggregator.onTrade(makeTrade(SYMBOL, 102, 1, 2));  // bar closes
  aggregator.onTrade(makeTrade(SYMBOL, 103, 1, 3));  // new bar starts

  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(102.0));
  EXPECT_EQ(result[0].tradeCount.raw(), 3);  // Raw trade count, not scaled
}

// ============================================================================
// VolumeBarPolicy Tests
// ============================================================================

TEST(VolumeBarPolicyTest, ClosesOnVolumeThreshold)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  // Volume threshold of 300 (notional = price * qty)
  BarAggregator<VolumeBarPolicy> aggregator(VolumeBarPolicy::fromDouble(300.0), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));  // volume = 100
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 1));  // volume = 200
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 2));  // volume = 300, closes
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 3));  // new bar

  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].volume, Volume::fromDouble(300.0));
}

// ============================================================================
// RenkoBarPolicy Tests
// ============================================================================

TEST(RenkoBarPolicyTest, CreatesBricksOnPriceMove)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  // Brick size of 10
  BarAggregator<RenkoBarPolicy> aggregator(RenkoBarPolicy::fromDouble(10.0), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 1));  // within brick
  aggregator.onTrade(makeTrade(SYMBOL, 111, 1, 2));  // brick closes (moved 11 from 100)

  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
}

// ============================================================================
// RangeBarPolicy Tests
// ============================================================================

TEST(RangeBarPolicyTest, ClosesOnRangeBreak)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  // Range size of 10
  BarAggregator<RangeBarPolicy> aggregator(RangeBarPolicy::fromDouble(10.0), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 1));  // range = 5
  aggregator.onTrade(makeTrade(SYMBOL, 95, 1, 2));   // range = 10, closes

  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].high, Price::fromDouble(105.0));
  EXPECT_EQ(result[0].low, Price::fromDouble(100.0));  // Note: didn't update to 95 before close check
}

// ============================================================================
// BarAggregator Integration Tests
// ============================================================================

TEST(BarAggregatorTest, EmitsBarEventOnClose)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 65));  // flush

  bus.stop();

  ASSERT_GE(result.size(), 1);
}

TEST(BarAggregatorTest, HandlesMultipleSymbols)
{
  std::vector<Bar> bars;
  std::vector<SymbolId> symbols;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(bars, &symbols);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(1, 10, 1, 0));
  aggregator.onTrade(makeTrade(2, 20, 2, 10));
  aggregator.onTrade(makeTrade(1, 12, 1, 30));
  aggregator.onTrade(makeTrade(2, 18, 1, 40));

  aggregator.stop();  // flush all
  bus.stop();

  ASSERT_EQ(bars.size(), 2);

  auto it1 = std::find_if(symbols.begin(), symbols.end(), [](SymbolId s)
                          { return s == 1; });
  auto it2 = std::find_if(symbols.begin(), symbols.end(), [](SymbolId s)
                          { return s == 2; });

  ASSERT_TRUE(it1 != symbols.end());
  ASSERT_TRUE(it2 != symbols.end());

  EXPECT_EQ(bars[0].volume + bars[1].volume, Volume::fromDouble(10 * 1 + 12 * 1 + 20 * 2 + 18 * 1));
}

TEST(BarAggregatorTest, MaintainsOHLCVCorrectly)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));   // O=100, H=100, L=100, C=100
  aggregator.onTrade(makeTrade(SYMBOL, 110, 1, 10));  // O=100, H=110, L=100, C=110
  aggregator.onTrade(makeTrade(SYMBOL, 90, 1, 20));   // O=100, H=110, L=90, C=90
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 30));  // O=100, H=110, L=90, C=105

  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].open, Price::fromDouble(100.0));
  EXPECT_EQ(result[0].high, Price::fromDouble(110.0));
  EXPECT_EQ(result[0].low, Price::fromDouble(90.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(105.0));
}

TEST(BarAggregatorTest, TracksBuyVolumeForDelta)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0, InstrumentType::Spot, true));    // buy
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 10, InstrumentType::Spot, false));  // sell
  aggregator.onTrade(makeTrade(SYMBOL, 100, 2, 20, InstrumentType::Spot, true));   // buy

  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 1);
  // Total volume = 100*1 + 100*1 + 100*2 = 400
  // Buy volume = 100*1 + 100*2 = 300
  EXPECT_EQ(result[0].volume, Volume::fromDouble(400.0));
  EXPECT_EQ(result[0].buyVolume, Volume::fromDouble(300.0));
}

TEST(BarAggregatorTest, InstrumentTypeIsPropagated)
{
  std::vector<InstrumentType> types;

  class StrategyWithInstrument : public IStrategy
  {
   public:
    explicit StrategyWithInstrument(std::vector<InstrumentType>& out) : _out(out) {}

    SubscriberId id() const override { return 99; }

    void onBar(const BarEvent& event) override { _out.push_back(event.instrument); }

   private:
    std::vector<InstrumentType>& _out;
  };

  BarBus bus;
  bus.enableDrainOnStop();

  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<StrategyWithInstrument>(types);
  bus.subscribe(strat.get());

  SymbolRegistry registry;
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = "BTC-FUT-TEST";
  info.type = InstrumentType::Future;
  SymbolId sid = registry.registerSymbol(info);

  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(sid, 120, 1, 10, InstrumentType::Future));
  aggregator.stop();
  bus.stop();

  ASSERT_EQ(types.size(), 1);
  EXPECT_EQ(types[0], InstrumentType::Future);
}

TEST(BarAggregatorTest, DoubleStartClearsOldState)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();

  aggregator.start();
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.start();  // clears previous state
  aggregator.onTrade(makeTrade(SYMBOL, 105, 2, 65));
  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].open, Price::fromDouble(105.0));
  EXPECT_EQ(result[0].volume, Volume::fromDouble(105.0 * 2));
  EXPECT_EQ(result[0].startTime, ts(60));
}

// ============================================================================
// MultiTimeframeAggregator Tests
// ============================================================================

TEST(MultiTimeframeAggregatorTest, ProducesMultipleTimeframes)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();

  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(std::chrono::seconds(60));   // M1
  aggregator.addTimeInterval(std::chrono::seconds(300));  // M5

  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());

  bus.start();
  aggregator.start();

  // Generate trades spanning multiple M1 and one M5
  for (int i = 0; i < 10; ++i)
  {
    aggregator.onTrade(makeTrade(SYMBOL, 100 + i, 1, i * 35));
  }

  aggregator.stop();
  bus.stop();

  // Should have M1 bars and M5 bars
  EXPECT_GT(result.size(), 2);  // At least some bars from both timeframes
}

TEST(MultiTimeframeAggregatorTest, MixedBarTypes)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();

  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(std::chrono::seconds(60));
  aggregator.addTickInterval(5);  // 5-tick bars

  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());

  bus.start();
  aggregator.start();

  // Generate 10 trades
  for (int i = 0; i < 10; ++i)
  {
    aggregator.onTrade(makeTrade(SYMBOL, 100 + i, 1, i * 5));
  }

  aggregator.stop();
  bus.stop();

  // Should have at least one 5-tick bar closed
  EXPECT_GE(result.size(), 1);
}

// ============================================================================
// SingleTradeBar Test
// ============================================================================

TEST(BarAggregatorTest, SingleTradeBar)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  TimeBarAggregator aggregator(TimeBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();
  aggregator.onTrade(makeTrade(SYMBOL, 123, 1, 5));
  aggregator.stop();  // flush
  bus.stop();

  ASSERT_EQ(result.size(), 1);
  const auto& bar = result[0];
  EXPECT_EQ(bar.open, Price::fromDouble(123.0));
  EXPECT_EQ(bar.high, Price::fromDouble(123.0));
  EXPECT_EQ(bar.low, Price::fromDouble(123.0));
  EXPECT_EQ(bar.close, Price::fromDouble(123.0));
  EXPECT_EQ(bar.volume, Volume::fromDouble(123.0 * 1));
}

// ============================================================================
// VolumeProfile Tests
// ============================================================================

TEST(VolumeProfileTest, CalculatesPOC)
{
  VolumeProfile<64> profile;
  profile.setTickSize(Price::fromDouble(1.0));

  // Add trades at different prices
  profile.addTrade(makeTrade(SYMBOL, 100, 1, 0));  // vol = 100
  profile.addTrade(makeTrade(SYMBOL, 100, 2, 1));  // vol = 200, total @ 100 = 300
  profile.addTrade(makeTrade(SYMBOL, 101, 1, 2));  // vol = 101
  profile.addTrade(makeTrade(SYMBOL, 102, 1, 3));  // vol = 102

  // POC should be 100 (highest volume)
  EXPECT_EQ(profile.poc(), Price::fromDouble(100.0));
  EXPECT_EQ(profile.numLevels(), 3);
}

TEST(VolumeProfileTest, ValueArea70Percent)
{
  VolumeProfile<64> profile;
  profile.setTickSize(Price::fromDouble(1.0));

  // Create a profile with most volume in the middle
  profile.addTrade(makeTrade(SYMBOL, 98, 1, 0));   // vol = 98
  profile.addTrade(makeTrade(SYMBOL, 99, 2, 1));   // vol = 198
  profile.addTrade(makeTrade(SYMBOL, 100, 5, 2));  // vol = 500 (POC)
  profile.addTrade(makeTrade(SYMBOL, 101, 2, 3));  // vol = 202
  profile.addTrade(makeTrade(SYMBOL, 102, 1, 4));  // vol = 102

  // Total = 1100, 70% = 770
  // POC at 100 (500), expand to include 99 (198) and 101 (202) = 900 > 770
  EXPECT_EQ(profile.poc(), Price::fromDouble(100.0));
  EXPECT_GE(profile.valueAreaLow().raw(), Price::fromDouble(99.0).raw());
  EXPECT_LE(profile.valueAreaHigh().raw(), Price::fromDouble(101.0).raw());
}

TEST(VolumeProfileTest, TracksDelta)
{
  VolumeProfile<64> profile;
  profile.setTickSize(Price::fromDouble(1.0));

  profile.addTrade(makeTrade(SYMBOL, 100, 1, 0, InstrumentType::Spot, true));   // buy 100
  profile.addTrade(makeTrade(SYMBOL, 100, 2, 1, InstrumentType::Spot, false));  // sell 200

  // Total = 300, buy = 100, sell = 200, delta = -100
  EXPECT_EQ(profile.totalVolume(), Volume::fromDouble(300.0));
  EXPECT_EQ(profile.totalDelta(), Volume::fromRaw(-100 * Volume::Scale));
}

// ============================================================================
// FootprintBar Tests
// ============================================================================

TEST(FootprintBarTest, TracksBidAskVolume)
{
  FootprintBar<32> footprint;
  footprint.setTickSize(Price::fromDouble(1.0));

  // Buy order (aggressive buyer lifting ask)
  footprint.addTrade(makeTrade(SYMBOL, 100, 2, 0, InstrumentType::Spot, true));
  // Sell order (aggressive seller hitting bid)
  footprint.addTrade(makeTrade(SYMBOL, 100, 3, 1, InstrumentType::Spot, false));

  const auto* level = footprint.levelAt(Price::fromDouble(100.0));
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->askVolume, Quantity::fromDouble(2.0));
  EXPECT_EQ(level->bidVolume, Quantity::fromDouble(3.0));
}

TEST(FootprintBarTest, CalculatesDelta)
{
  FootprintBar<32> footprint;
  footprint.setTickSize(Price::fromDouble(1.0));

  footprint.addTrade(makeTrade(SYMBOL, 100, 5, 0, InstrumentType::Spot, true));   // ask vol
  footprint.addTrade(makeTrade(SYMBOL, 100, 3, 1, InstrumentType::Spot, false));  // bid vol
  footprint.addTrade(makeTrade(SYMBOL, 101, 2, 2, InstrumentType::Spot, true));   // ask vol

  // Total delta = (5-3) + (2-0) = 4
  EXPECT_EQ(footprint.totalDelta(), Quantity::fromDouble(4.0));
  EXPECT_EQ(footprint.numLevels(), 2);
}

TEST(FootprintBarTest, IdentifiesPressureLevels)
{
  FootprintBar<32> footprint;
  footprint.setTickSize(Price::fromDouble(1.0));

  // Heavy buying at 100
  footprint.addTrade(makeTrade(SYMBOL, 100, 10, 0, InstrumentType::Spot, true));
  // Heavy selling at 99
  footprint.addTrade(makeTrade(SYMBOL, 99, 8, 1, InstrumentType::Spot, false));
  // Light activity at 101
  footprint.addTrade(makeTrade(SYMBOL, 101, 1, 2, InstrumentType::Spot, true));

  EXPECT_EQ(footprint.highestBuyingPressure(), Price::fromDouble(100.0));
  EXPECT_EQ(footprint.highestSellingPressure(), Price::fromDouble(99.0));
}

// ============================================================================
// MarketProfile Tests
// ============================================================================

TEST(MarketProfileTest, CalculatesPOC)
{
  MarketProfile<64, 26> profile;
  profile.setTickSize(Price::fromDouble(1.0));
  profile.setPeriodDuration(std::chrono::minutes(30));
  profile.setSessionStart(0);

  // Period A (0-30min): trades at 100, 101
  profile.addTrade(makeTrade(SYMBOL, 100, 1, 0));
  profile.addTrade(makeTrade(SYMBOL, 101, 1, 60));
  profile.addTrade(makeTrade(SYMBOL, 100, 1, 120));

  // Period B (30-60min): trades at 100, 102
  profile.addTrade(makeTrade(SYMBOL, 100, 1, 30 * 60));
  profile.addTrade(makeTrade(SYMBOL, 102, 1, 35 * 60));

  // 100 has 2 TPOs (A and B), others have 1
  EXPECT_EQ(profile.poc(), Price::fromDouble(100.0));
}

TEST(MarketProfileTest, TracksInitialBalance)
{
  MarketProfile<64, 26> profile;
  profile.setTickSize(Price::fromDouble(1.0));
  profile.setPeriodDuration(std::chrono::minutes(30));
  profile.setSessionStart(0);

  // Period A
  profile.addTrade(makeTrade(SYMBOL, 100, 1, 0));
  profile.addTrade(makeTrade(SYMBOL, 105, 1, 10 * 60));

  // Period B
  profile.addTrade(makeTrade(SYMBOL, 98, 1, 30 * 60));
  profile.addTrade(makeTrade(SYMBOL, 103, 1, 40 * 60));

  // Period C (outside IB)
  profile.addTrade(makeTrade(SYMBOL, 95, 1, 60 * 60));
  profile.addTrade(makeTrade(SYMBOL, 110, 1, 70 * 60));

  // IB should be 98-105 (A+B range)
  EXPECT_EQ(profile.initialBalanceLow(), Price::fromDouble(98.0));
  EXPECT_EQ(profile.initialBalanceHigh(), Price::fromDouble(105.0));
}

TEST(MarketProfileTest, IdentifiesSinglePrints)
{
  MarketProfile<64, 26> profile;
  profile.setTickSize(Price::fromDouble(1.0));
  profile.setPeriodDuration(std::chrono::minutes(30));
  profile.setSessionStart(0);

  // 100 appears in A and B
  profile.addTrade(makeTrade(SYMBOL, 100, 1, 0));
  profile.addTrade(makeTrade(SYMBOL, 100, 1, 30 * 60));

  // 105 appears only in A (single print)
  profile.addTrade(makeTrade(SYMBOL, 105, 1, 10 * 60));

  const auto* level100 = profile.levelAt(Price::fromDouble(100.0));
  const auto* level105 = profile.levelAt(Price::fromDouble(105.0));

  ASSERT_NE(level100, nullptr);
  ASSERT_NE(level105, nullptr);

  EXPECT_FALSE(level100->isSinglePrint());
  EXPECT_TRUE(level105->isSinglePrint());

  auto [count, singles] = profile.singlePrints();
  EXPECT_EQ(count, 1);
  EXPECT_EQ(singles[0], Price::fromDouble(105.0));
}

TEST(MarketProfileTest, IdentifiesPoorHighLow)
{
  MarketProfile<64, 26> profile;
  profile.setTickSize(Price::fromDouble(1.0));
  profile.setPeriodDuration(std::chrono::minutes(30));
  profile.setSessionStart(0);

  // Period A: 100-102
  profile.addTrade(makeTrade(SYMBOL, 100, 1, 0));
  profile.addTrade(makeTrade(SYMBOL, 101, 1, 5 * 60));
  profile.addTrade(makeTrade(SYMBOL, 102, 1, 10 * 60));

  // Period B: 100-101 (no new high, 102 stays single print = poor high)
  profile.addTrade(makeTrade(SYMBOL, 100, 1, 30 * 60));
  profile.addTrade(makeTrade(SYMBOL, 101, 1, 35 * 60));

  EXPECT_TRUE(profile.isPoorHigh());  // 102 is single print at high
  EXPECT_FALSE(profile.isPoorLow());  // 100 has 2 TPOs
}
