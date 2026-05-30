#include <gtest/gtest.h>

#include <chrono>

#include "flox/engine/symbol_registry.h"
#include "flox/position/portfolio_greeks.h"
#include "flox/position/position_group.h"
#include "flox/pricing/greeks.h"

using namespace flox;

namespace
{
int64_t expiryNs(double daysFromNow, int64_t nowNs)
{
  return nowNs + static_cast<int64_t>(daysFromNow * 24.0 * 3600.0 * 1e9);
}

SymbolId registerOption(SymbolRegistry& reg, const std::string& name, OptionType type,
                        double strike, int64_t expNs)
{
  SymbolInfo info;
  info.exchange = "deribit";
  info.symbol = name;
  info.type = InstrumentType::Option;
  info.strike = Price::fromDouble(strike);
  info.optionType = type;
  info.expiry = TimePoint(std::chrono::nanoseconds(expNs));
  info.settlementType = SettlementType::Cash;
  return reg.registerSymbol(info);
}

SymbolId registerPerp(SymbolRegistry& reg, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "bybit";
  info.symbol = name;
  info.type = InstrumentType::Future;
  return reg.registerSymbol(info);
}

constexpr int64_t kNow = 1'700'000'000'000'000'000LL;
}  // namespace

TEST(PortfolioGreeksTest, SingleLongCallMatchesAnalytic)
{
  SymbolRegistry reg;
  SymbolId call = registerOption(reg, "BTC-CALL", OptionType::CALL, 100.0, expiryNs(30.0, kNow));

  PositionGroupTracker positions;
  positions.openPosition(1, call, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(3.0));

  PortfolioGreeksAggregator agg(reg);
  auto pg = agg.compute(kNow, positions, [](SymbolId)
                        { return 100.0; }, [](SymbolId)
                        { return 0.6; });

  const double t = 30.0 / 365.0;
  auto g = pricing::greeks(OptionType::CALL, 100.0, 100.0, t, 0.0, 0.0, 0.6);
  EXPECT_NEAR(pg.delta, g.delta * 3.0, 1e-6);
  EXPECT_NEAR(pg.gamma, g.gamma * 3.0, 1e-6);
  EXPECT_NEAR(pg.vega, g.vega * 3.0, 1e-6);
  EXPECT_NEAR(pg.theta, g.theta * 3.0, 1e-6);
}

TEST(PortfolioGreeksTest, ShortOptionFlipsSign)
{
  SymbolRegistry reg;
  SymbolId call = registerOption(reg, "BTC-CALL", OptionType::CALL, 100.0, expiryNs(30.0, kNow));

  PositionGroupTracker positions;
  positions.openPosition(1, call, Side::SELL, Price::fromDouble(5.0), Quantity::fromDouble(3.0));

  PortfolioGreeksAggregator agg(reg);
  auto pg = agg.compute(kNow, positions, [](SymbolId)
                        { return 100.0; }, [](SymbolId)
                        { return 0.6; });

  const double t = 30.0 / 365.0;
  auto g = pricing::greeks(OptionType::CALL, 100.0, 100.0, t, 0.0, 0.0, 0.6);
  EXPECT_NEAR(pg.delta, -g.delta * 3.0, 1e-6);
  EXPECT_NEAR(pg.gamma, -g.gamma * 3.0, 1e-6);  // short gamma is negative
}

TEST(PortfolioGreeksTest, PerpAddsLinearDelta)
{
  SymbolRegistry reg;
  SymbolId perp = registerPerp(reg, "BTCUSDT");

  PositionGroupTracker positions;
  positions.openPosition(1, perp, Side::BUY, Price::fromDouble(100.0), Quantity::fromDouble(2.5));

  PortfolioGreeksAggregator agg(reg);
  auto pg = agg.compute(kNow, positions, [](SymbolId)
                        { return 100.0; }, [](SymbolId)
                        { return 0.6; });
  EXPECT_NEAR(pg.delta, 2.5, 1e-9);  // perp delta = qty
  EXPECT_NEAR(pg.gamma, 0.0, 1e-12);
  EXPECT_NEAR(pg.vega, 0.0, 1e-12);
}

TEST(PortfolioGreeksTest, DeltaNeutralCallHedgedWithPerp)
{
  SymbolRegistry reg;
  SymbolId call = registerOption(reg, "BTC-CALL", OptionType::CALL, 100.0, expiryNs(30.0, kNow));
  SymbolId perp = registerPerp(reg, "BTCUSDT");

  const double t = 30.0 / 365.0;
  auto g = pricing::greeks(OptionType::CALL, 100.0, 100.0, t, 0.0, 0.0, 0.6);
  const double callQty = 4.0;
  const double hedgeUnits = g.delta * callQty;  // short this many perp units

  PositionGroupTracker positions;
  positions.openPosition(1, call, Side::BUY, Price::fromDouble(5.0), Quantity::fromDouble(callQty));
  positions.openPosition(2, perp, Side::SELL, Price::fromDouble(100.0),
                         Quantity::fromDouble(hedgeUnits));

  PortfolioGreeksAggregator agg(reg);
  auto pg = agg.compute(kNow, positions, [](SymbolId)
                        { return 100.0; }, [](SymbolId)
                        { return 0.6; });
  EXPECT_NEAR(pg.delta, 0.0, 1e-6);  // delta-neutral
  EXPECT_GT(pg.gamma, 0.0);          // still long gamma from the call
}

TEST(PortfolioGreeksTest, VegaBucketedByTenor)
{
  SymbolRegistry reg;
  SymbolId nearCall = registerOption(reg, "BTC-NEAR", OptionType::CALL, 100.0, expiryNs(10.0, kNow));
  SymbolId farCall = registerOption(reg, "BTC-FAR", OptionType::CALL, 100.0, expiryNs(120.0, kNow));

  PositionGroupTracker positions;
  positions.openPosition(1, nearCall, Side::BUY, Price::fromDouble(2.0), Quantity::fromDouble(1.0));
  positions.openPosition(2, farCall, Side::BUY, Price::fromDouble(8.0), Quantity::fromDouble(1.0));

  PortfolioGreeksAggregator agg(reg);
  auto pg = agg.compute(kNow, positions, [](SymbolId)
                        { return 100.0; }, [](SymbolId)
                        { return 0.6; });

  const double shortVega = agg.vegaInTenor(VegaTenor::Short);
  const double longVega = agg.vegaInTenor(VegaTenor::Long);
  EXPECT_GT(shortVega, 0.0);
  EXPECT_GT(longVega, 0.0);
  EXPECT_NEAR(shortVega + longVega, pg.vega, 1e-6);
  EXPECT_NEAR(agg.vegaInTenor(VegaTenor::Medium), 0.0, 1e-9);  // nothing in 30-90d
}
