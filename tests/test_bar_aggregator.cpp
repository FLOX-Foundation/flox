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
#include "flox/aggregator/policies/heikin_ashi_bar_policy.h"
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

TEST(TimeBarPolicyTest, SupportsSubSecondIntervals)
{
  // Helper for millisecond timestamps
  auto tsMs = [](int64_t ms)
  { return TimePoint(std::chrono::milliseconds(ms)); };
  auto makeTradeMs = [](SymbolId symbol, double price, double qty, int64_t ms)
  {
    TradeEvent event;
    event.trade.symbol = symbol;
    event.trade.instrument = InstrumentType::Spot;
    event.trade.price = Price::fromDouble(price);
    event.trade.quantity = Quantity::fromDouble(qty);
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = std::chrono::nanoseconds(std::chrono::milliseconds(ms)).count();
    return event;
  };

  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  // 100ms bars
  TimeBarAggregator aggregator(TimeBarPolicy(std::chrono::milliseconds(100)), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  // Trades within first 100ms bar (0-100ms)
  aggregator.onTrade(makeTradeMs(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTradeMs(SYMBOL, 105, 1, 50));

  // Trade at 100ms triggers flush
  aggregator.onTrade(makeTradeMs(SYMBOL, 110, 1, 100));

  // Trade at 200ms triggers another flush
  aggregator.onTrade(makeTradeMs(SYMBOL, 115, 1, 200));

  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 3);
  EXPECT_EQ(result[0].startTime, tsMs(0));
  EXPECT_EQ(result[0].endTime, tsMs(100));
  EXPECT_EQ(result[0].close, Price::fromDouble(105.0));

  EXPECT_EQ(result[1].startTime, tsMs(100));
  EXPECT_EQ(result[1].endTime, tsMs(200));
  EXPECT_EQ(result[1].close, Price::fromDouble(110.0));

  EXPECT_EQ(result[2].startTime, tsMs(200));
  EXPECT_EQ(result[2].endTime, tsMs(300));
  EXPECT_EQ(result[2].close, Price::fromDouble(115.0));
}

TEST(TimeBarPolicyTest, SupportsMicrosecondIntervals)
{
  // Helper for microsecond timestamps
  auto tsUs = [](int64_t us)
  { return TimePoint(std::chrono::microseconds(us)); };
  auto makeTradeUs = [](SymbolId symbol, double price, double qty, int64_t us)
  {
    TradeEvent event;
    event.trade.symbol = symbol;
    event.trade.instrument = InstrumentType::Spot;
    event.trade.price = Price::fromDouble(price);
    event.trade.quantity = Quantity::fromDouble(qty);
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = std::chrono::nanoseconds(std::chrono::microseconds(us)).count();
    return event;
  };

  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  // 100µs bars
  TimeBarAggregator aggregator(TimeBarPolicy(std::chrono::microseconds(100)), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  // Trades within first 100µs bar
  aggregator.onTrade(makeTradeUs(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTradeUs(SYMBOL, 101, 1, 50));

  // Trade at 100µs triggers flush
  aggregator.onTrade(makeTradeUs(SYMBOL, 102, 1, 100));

  // Trade at 200µs triggers another flush
  aggregator.onTrade(makeTradeUs(SYMBOL, 103, 1, 200));

  aggregator.stop();
  bus.stop();

  ASSERT_EQ(result.size(), 3);
  EXPECT_EQ(result[0].startTime, tsUs(0));
  EXPECT_EQ(result[0].endTime, tsUs(100));
  EXPECT_EQ(result[0].close, Price::fromDouble(101.0));

  EXPECT_EQ(result[1].startTime, tsUs(100));
  EXPECT_EQ(result[1].endTime, tsUs(200));
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
// HeikinAshiBarPolicy Tests
// ============================================================================

TEST(HeikinAshiBarPolicyTest, CalculatesHeikinAshiOHLC)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  HeikinAshiBarAggregator aggregator(HeikinAshiBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  // First bar: O=100, H=110, L=95, C=105
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 110, 1, 10));
  aggregator.onTrade(makeTrade(SYMBOL, 95, 1, 20));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 30));

  // Trigger close
  aggregator.onTrade(makeTrade(SYMBOL, 108, 1, 65));

  bus.stop();

  ASSERT_EQ(result.size(), 1);

  // Heikin-Ashi formulas:
  // HA_Close = (O + H + L + C) / 4 = (100 + 110 + 95 + 105) / 4 = 102.5
  // HA_Open (first bar) = (rawOpen + rawClose) / 2 = (100 + 105) / 2 = 102.5
  // HA_High = max(rawHigh, HA_Open, HA_Close) = max(110, 102.5, 102.5) = 110
  // HA_Low = min(rawLow, HA_Open, HA_Close) = min(95, 102.5, 102.5) = 95
  EXPECT_EQ(result[0].high, Price::fromDouble(110.0));
  EXPECT_EQ(result[0].low, Price::fromDouble(95.0));
  EXPECT_EQ(result[0].close, Price::fromDouble(102.5));
  EXPECT_EQ(result[0].open, Price::fromDouble(102.5));
}

TEST(HeikinAshiBarPolicyTest, UsesTimeBasedIntervals)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  HeikinAshiBarAggregator aggregator(HeikinAshiBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 30));
  aggregator.onTrade(makeTrade(SYMBOL, 110, 1, 65));  // triggers flush
  aggregator.onTrade(makeTrade(SYMBOL, 115, 1, 90));

  bus.stop();

  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].startTime, ts(0));
  EXPECT_EQ(result[0].endTime, ts(60));
}

TEST(HeikinAshiBarPolicyTest, ChainedBarsUsesPreviousHAValues)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  HeikinAshiBarAggregator aggregator(HeikinAshiBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  // First bar
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 110, 1, 30));

  // Second bar (trigger flush of first)
  aggregator.onTrade(makeTrade(SYMBOL, 115, 1, 65));
  aggregator.onTrade(makeTrade(SYMBOL, 120, 1, 90));

  // Third bar (trigger flush of second)
  aggregator.onTrade(makeTrade(SYMBOL, 125, 1, 125));

  bus.stop();

  ASSERT_EQ(result.size(), 2);

  // Second bar's HA_Open should be based on first bar's HA values
  // First bar: HA_Close = (100+110+100+110)/4 = 105, HA_Open = (100+110)/2 = 105
  // Second bar: HA_Open = (prev_HA_Open + prev_HA_Close)/2 = (105+105)/2 = 105
  EXPECT_EQ(result[1].open, Price::fromDouble(105.0));
}

TEST(HeikinAshiBarPolicyTest, SmoothsTrends)
{
  // Heikin-Ashi is known for smoothing out noise and showing clearer trends
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();
  HeikinAshiBarAggregator aggregator(HeikinAshiBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  // Uptrend with noise
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 30));
  aggregator.onTrade(makeTrade(SYMBOL, 110, 1, 65));   // bar 1 flush
  aggregator.onTrade(makeTrade(SYMBOL, 108, 1, 90));   // pullback
  aggregator.onTrade(makeTrade(SYMBOL, 115, 1, 125));  // bar 2 flush
  aggregator.onTrade(makeTrade(SYMBOL, 120, 1, 150));

  aggregator.stop();
  bus.stop();

  // In uptrend, HA bars should have close >= open (bullish)
  ASSERT_GE(result.size(), 2);
  for (const auto& bar : result)
  {
    EXPECT_GE(bar.close.raw(), bar.open.raw());
  }
}

TEST(HeikinAshiBarPolicyTest, MultiSymbolIndependentState)
{
  // Each symbol should maintain independent HA state
  constexpr SymbolId SYM_A = 1;
  constexpr SymbolId SYM_B = 2;

  std::vector<Bar> result;
  std::vector<SymbolId> symbols;
  BarBus bus;
  bus.enableDrainOnStop();
  HeikinAshiBarAggregator aggregator(HeikinAshiBarPolicy(INTERVAL), &bus);
  auto strat = std::make_unique<TestStrategy>(result, &symbols);
  bus.subscribe(strat.get());
  bus.start();
  aggregator.start();

  // Bar 1 for both symbols with different prices
  // SYM_A: price 100, SYM_B: price 200
  aggregator.onTrade(makeTrade(SYM_A, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYM_B, 200, 1, 0));

  // Flush bar 1, start bar 2
  aggregator.onTrade(makeTrade(SYM_A, 110, 1, 65));  // SYM_A bar 1 flush
  aggregator.onTrade(makeTrade(SYM_B, 220, 1, 65));  // SYM_B bar 1 flush

  // More trades in bar 2
  aggregator.onTrade(makeTrade(SYM_A, 120, 1, 90));
  aggregator.onTrade(makeTrade(SYM_B, 240, 1, 90));

  // Flush bar 2
  aggregator.onTrade(makeTrade(SYM_A, 130, 1, 130));
  aggregator.onTrade(makeTrade(SYM_B, 260, 1, 130));

  aggregator.stop();
  bus.stop();

  // Should have 6 bars: 3 for SYM_A, 3 for SYM_B (including drained bars on stop)
  ASSERT_EQ(result.size(), 6);

  // Find bars by symbol
  std::vector<Bar> barsA, barsB;
  for (size_t i = 0; i < result.size(); ++i)
  {
    if (symbols[i] == SYM_A)
    {
      barsA.push_back(result[i]);
    }
    else
    {
      barsB.push_back(result[i]);
    }
  }

  ASSERT_EQ(barsA.size(), 3);
  ASSERT_EQ(barsB.size(), 3);

  // SYM_B prices should be roughly 2x SYM_A prices (independent state)
  // If state was shared, the HA values would be mixed and incorrect
  EXPECT_GT(barsB[0].close.raw(), barsA[0].close.raw() * 1.5);
  EXPECT_GT(barsB[1].close.raw(), barsA[1].close.raw() * 1.5);

  // Each symbol's bar 2 should use its own prev HA values
  // HA_Open for bar 2 = (prev_HA_Open + prev_HA_Close) / 2
  // SYM_A bar 2 HA_Open should be around 100-110 range
  // SYM_B bar 2 HA_Open should be around 200-220 range
  EXPECT_LT(barsA[1].open.raw(), Price::fromDouble(150).raw());
  EXPECT_GT(barsB[1].open.raw(), Price::fromDouble(150).raw());
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

  profile.addTrade(makeTrade(SYMBOL, 100, 1, 0, InstrumentType::Spot, true));   // buy notional=100*1=100
  profile.addTrade(makeTrade(SYMBOL, 100, 2, 1, InstrumentType::Spot, false));  // sell notional=100*2=200

  // Total notional = 300, buy = 100, sell = 200, delta = -100
  EXPECT_EQ(profile.totalVolume(), Volume::fromDouble(300.0));
  EXPECT_EQ(profile.totalDelta(), Volume::fromDouble(-100.0));
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

// ============================================================================
// MultiTimeframeAggregator Edge Case Tests
// ============================================================================

TEST(MultiTimeframeAggregatorTest, NoSlotsAddedDoesNothing)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();

  MultiTimeframeAggregator<4> aggregator(&bus);
  // No slots added

  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());

  bus.start();
  aggregator.start();

  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 65));

  aggregator.stop();
  bus.stop();

  EXPECT_EQ(result.size(), 0);  // No bars without slots
  EXPECT_EQ(aggregator.numTimeframes(), 0);
}

TEST(MultiTimeframeAggregatorTest, MaxSlotsReachedReturnsMaxIndex)
{
  BarBus bus;
  MultiTimeframeAggregator<2> aggregator(&bus);  // Max 2 slots

  size_t idx0 = aggregator.addTimeInterval(std::chrono::seconds(60));
  size_t idx1 = aggregator.addTimeInterval(std::chrono::seconds(300));
  size_t idx2 = aggregator.addTimeInterval(std::chrono::seconds(900));  // Overflow

  EXPECT_EQ(idx0, 0);
  EXPECT_EQ(idx1, 1);
  EXPECT_EQ(idx2, 2);  // Returns MaxTimeframes (out of bounds indicator)
  EXPECT_EQ(aggregator.numTimeframes(), 2);
}

TEST(MultiTimeframeAggregatorTest, AllVolumeSlots)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();

  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addVolumeInterval(100.0);   // 100 notional
  aggregator.addVolumeInterval(500.0);   // 500 notional
  aggregator.addVolumeInterval(1000.0);  // 1000 notional

  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());

  bus.start();
  aggregator.start();

  // Generate trades: price=100, qty=1 => notional=100 per trade
  for (int i = 0; i < 12; ++i)
  {
    aggregator.onTrade(makeTrade(SYMBOL, 100, 1, i));
  }

  aggregator.stop();
  bus.stop();

  // 100 threshold: 12 trades = 12 bars
  // 500 threshold: 12 trades = 2 full bars + 1 partial
  // 1000 threshold: 12 trades = 1 full bar + 1 partial
  // Total includes flushed on stop
  EXPECT_GT(result.size(), 12);  // At least 12 from 100-threshold slot
}

TEST(MultiTimeframeAggregatorTest, SingleTradeAcrossAllTimeframes)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();

  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(std::chrono::seconds(60));
  aggregator.addTimeInterval(std::chrono::seconds(300));
  aggregator.addTickInterval(10);
  aggregator.addVolumeInterval(1000.0);

  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());

  bus.start();
  aggregator.start();

  // Single trade
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));

  aggregator.stop();
  bus.stop();

  // All 4 timeframes should flush their partial bars
  EXPECT_EQ(result.size(), 4);

  // All bars should have same OHLC from single trade
  for (const auto& bar : result)
  {
    EXPECT_EQ(bar.open, Price::fromDouble(100.0));
    EXPECT_EQ(bar.high, Price::fromDouble(100.0));
    EXPECT_EQ(bar.low, Price::fromDouble(100.0));
    EXPECT_EQ(bar.close, Price::fromDouble(100.0));
  }
}

TEST(MultiTimeframeAggregatorTest, StartStopWithoutTradesEmitsNothing)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();

  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(std::chrono::seconds(60));
  aggregator.addTickInterval(10);

  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());

  bus.start();
  aggregator.start();
  // No trades
  aggregator.stop();
  bus.stop();

  EXPECT_EQ(result.size(), 0);  // No bars without trades
}

TEST(MultiTimeframeAggregatorTest, MultiSymbolMultiTimeframe)
{
  std::vector<Bar> bars;
  std::vector<SymbolId> symbols;
  BarBus bus;
  bus.enableDrainOnStop();

  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(std::chrono::seconds(60));
  aggregator.addTickInterval(3);

  auto strat = std::make_unique<TestStrategy>(bars, &symbols);
  bus.subscribe(strat.get());

  bus.start();
  aggregator.start();

  // 2 symbols, interleaved trades
  for (int i = 0; i < 6; ++i)
  {
    aggregator.onTrade(makeTrade(1, 100 + i, 1, i * 10));
    aggregator.onTrade(makeTrade(2, 200 + i, 1, i * 10));
  }

  aggregator.stop();
  bus.stop();

  // 6 trades per symbol:
  // - TickInterval(3): 2 full bars per symbol (6/3=2)
  // - TimeInterval(60): 1 partial bar per symbol flushed on stop
  // Total: (2+1) * 2 symbols = 6 bars + potential more tick bars
  EXPECT_GE(bars.size(), 4);  // At least 2 tick bars per symbol

  // Verify both symbols present
  auto count1 = std::count(symbols.begin(), symbols.end(), 1);
  auto count2 = std::count(symbols.begin(), symbols.end(), 2);
  EXPECT_GT(count1, 0);
  EXPECT_GT(count2, 0);
}

TEST(MultiTimeframeAggregatorTest, TimeframesReturnsCorrectIds)
{
  BarBus bus;
  MultiTimeframeAggregator<8> aggregator(&bus);

  aggregator.addTimeInterval(std::chrono::seconds(60));
  aggregator.addTimeInterval(std::chrono::seconds(300));
  aggregator.addTickInterval(100);
  aggregator.addVolumeInterval(10000.0);

  auto tfs = aggregator.timeframes();

  EXPECT_EQ(aggregator.numTimeframes(), 4);
  EXPECT_EQ(tfs[0].type, BarType::Time);
  EXPECT_EQ(tfs[0].param, 60);
  EXPECT_EQ(tfs[1].type, BarType::Time);
  EXPECT_EQ(tfs[1].param, 300);
  EXPECT_EQ(tfs[2].type, BarType::Tick);
  EXPECT_EQ(tfs[2].param, 100);
  EXPECT_EQ(tfs[3].type, BarType::Volume);
  EXPECT_EQ(tfs[3].param, 10000);
}

TEST(MultiTimeframeAggregatorTest, DoubleStartClearsState)
{
  std::vector<Bar> result;
  BarBus bus;
  bus.enableDrainOnStop();

  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(std::chrono::seconds(60));

  auto strat = std::make_unique<TestStrategy>(result);
  bus.subscribe(strat.get());

  bus.start();
  aggregator.start();
  aggregator.onTrade(makeTrade(SYMBOL, 100, 1, 0));
  aggregator.onTrade(makeTrade(SYMBOL, 105, 1, 30));

  // Restart clears state
  aggregator.start();
  aggregator.onTrade(makeTrade(SYMBOL, 200, 1, 60));

  aggregator.stop();
  bus.stop();

  // Only the bar after restart should be emitted
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].open, Price::fromDouble(200.0));
}
