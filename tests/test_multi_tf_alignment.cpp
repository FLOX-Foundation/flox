/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace
{

class TestStrategy : public lrvx::Strategy
{
 public:
  using lrvx::Strategy::Strategy;
  int barCallbacks = 0;

 protected:
  void onSymbolBar(lrvx::SymbolContext&, const lrvx::BarEvent&) override
  {
    ++barCallbacks;
  }
};

lrvx::BarEvent makeTimeBar(lrvx::SymbolId sym, uint64_t intervalNs, double open, double close,
                           int64_t startNs)
{
  lrvx::BarEvent ev{};
  ev.symbol = sym;
  ev.barType = lrvx::BarType::Time;
  ev.barTypeParam = intervalNs;
  ev.bar.open = lrvx::Price::fromDouble(open);
  ev.bar.close = lrvx::Price::fromDouble(close);
  ev.bar.high = lrvx::Price::fromDouble(std::max(open, close));
  ev.bar.low = lrvx::Price::fromDouble(std::min(open, close));
  ev.bar.startTime = lrvx::TimePoint{std::chrono::nanoseconds{startNs}};
  ev.bar.endTime = lrvx::TimePoint{std::chrono::nanoseconds{startNs + (int64_t)intervalNs}};
  return ev;
}

lrvx::SymbolId addInfo(lrvx::SymbolRegistry& reg, const std::string& spelling)
{
  lrvx::SymbolInfo info;
  info.exchange = "test";
  info.symbol = spelling;
  info.type = lrvx::InstrumentType::Spot;
  info.tickSize = lrvx::Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

struct Fixture
{
  std::unique_ptr<lrvx::SymbolRegistry> reg = std::make_unique<lrvx::SymbolRegistry>();
  lrvx::SymbolId btc = addInfo(*reg, "BTCUSDT");
};

}  // namespace

TEST(MultiTfAlignment, ReturnsNulloptBeforeAnyBar)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  EXPECT_FALSE(s.lastClosedBar(f.btc, lrvx::BarType::Time, 60'000'000'000ull).has_value());
  EXPECT_TRUE(s.lastNClosedBars(f.btc, lrvx::BarType::Time, 60'000'000'000ull, 3).empty());
}

TEST(MultiTfAlignment, RingStoresLatestBar)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  const uint64_t H1 = 3'600'000'000'000ull;
  s.onBar(makeTimeBar(f.btc, H1, 100.0, 101.0, 0));
  s.onBar(makeTimeBar(f.btc, H1, 101.0, 102.5, (int64_t)H1));
  auto last = s.lastClosedBar(f.btc, lrvx::BarType::Time, H1);
  ASSERT_TRUE(last.has_value());
  EXPECT_NEAR(last->close.toDouble(), 102.5, 1e-9);
  EXPECT_EQ(s.barCallbacks, 2);
}

TEST(MultiTfAlignment, RingHonorsCapacity)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  s.setBarRingCapacity(3);
  const uint64_t M5 = 300'000'000'000ull;
  for (int i = 0; i < 7; ++i)
  {
    s.onBar(makeTimeBar(f.btc, M5, 100.0 + i, 100.5 + i, (int64_t)M5 * i));
  }
  auto last3 = s.lastNClosedBars(f.btc, lrvx::BarType::Time, M5, 3);
  ASSERT_EQ(last3.size(), 3u);
  EXPECT_NEAR(last3[0].close.toDouble(), 104.5, 1e-9);
  EXPECT_NEAR(last3[1].close.toDouble(), 105.5, 1e-9);
  EXPECT_NEAR(last3[2].close.toDouble(), 106.5, 1e-9);
}

TEST(MultiTfAlignment, MultiTfRingsStayIndependent)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  const uint64_t M5 = 300'000'000'000ull;
  const uint64_t H1 = 3'600'000'000'000ull;
  s.onBar(makeTimeBar(f.btc, M5, 100.0, 100.5, 0));
  s.onBar(makeTimeBar(f.btc, H1, 100.0, 105.0, 0));
  s.onBar(makeTimeBar(f.btc, M5, 100.5, 100.8, (int64_t)M5));
  auto m5 = s.lastClosedBar(f.btc, lrvx::BarType::Time, M5);
  auto h1 = s.lastClosedBar(f.btc, lrvx::BarType::Time, H1);
  ASSERT_TRUE(m5.has_value());
  ASSERT_TRUE(h1.has_value());
  EXPECT_NEAR(m5->close.toDouble(), 100.8, 1e-9);
  EXPECT_NEAR(h1->close.toDouble(), 105.0, 1e-9);
}

TEST(MultiTfAlignment, MultiSymbolRingsStayIndependent)
{
  auto reg = std::make_unique<lrvx::SymbolRegistry>();
  lrvx::SymbolId btc = addInfo(*reg, "BTCUSDT");
  lrvx::SymbolId eth = addInfo(*reg, "ETHUSDT");
  TestStrategy s(1, std::vector<lrvx::SymbolId>{btc, eth}, *reg);
  const uint64_t H4 = 14'400'000'000'000ull;
  s.onBar(makeTimeBar(btc, H4, 50000.0, 50100.0, 0));
  s.onBar(makeTimeBar(eth, H4, 3000.0, 3010.0, 0));
  auto btcBar = s.lastClosedBar(btc, lrvx::BarType::Time, H4);
  auto ethBar = s.lastClosedBar(eth, lrvx::BarType::Time, H4);
  ASSERT_TRUE(btcBar.has_value());
  ASSERT_TRUE(ethBar.has_value());
  EXPECT_NEAR(btcBar->close.toDouble(), 50100.0, 1e-9);
  EXPECT_NEAR(ethBar->close.toDouble(), 3010.0, 1e-9);
}
