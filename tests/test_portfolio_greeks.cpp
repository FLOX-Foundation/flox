#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <vector>

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

// W32-T012: compute() used to fold delta across positions.positions() in
// whatever order std::unordered_map<PositionId, IndividualPosition> happened
// to enumerate them, which is a hash-bucket order -- a property of the
// standard library, not of the data. libc++ and libstdc++ walk the same
// positions differently, so the aggregate delta below diverged in its low
// bits between machines built against different libraries, breaking
// bit-for-bit backtest reproduction. The fix collects the position ids and
// sorts them before folding.
TEST(PortfolioGreeksTest, DeltaFoldIsIndependentOfBucketOrder)
{
  SymbolRegistry reg;
  SymbolId perp = registerPerp(reg, "BTCUSDT");

  // Sixteen linear-leg positions, one per delta contribution. qty = 1 and
  // side = BUY on every one, so scale == contractMultiplier exactly, which
  // lets each contribution be set directly. The values span sixteen orders
  // of magnitude with alternating sign and non-round mantissas -- not a
  // single delicate cancellation, which a lucky bucket layout could still
  // fold to the same total regardless of order, but a value at every scale
  // a running sum could be at, so a fold that visits them in a different
  // order almost always rounds differently somewhere along the way. The
  // assertion below checks this rather than assuming it.
  std::vector<double> contributions = {
      1.7e16,
      -1.3e16,
      2.1e15,
      -1.9e14,
      3.3e13,
      -2.7e12,
      4.1e11,
      -1.1e10,
      9.9e8,
      -7.7e6,
      5.5e3,
      -6.6e2,
      8.8e1,
      -4.4,
      1.234567,
      -9.87654321,
  };
  ASSERT_EQ(contributions.size(), 16u);

  PositionGroupTracker positions;
  OrderId nextOrderId = 1;
  for (double c : contributions)
  {
    positions.openPosition(nextOrderId++, perp, Side::BUY, Price::fromDouble(0.0),
                           Quantity::fromDouble(1.0), c);
  }

  // Self-check: the map's own bucket-traversal order must not already be
  // ascending PositionId, or this test would exercise nothing -- the two
  // fold orders compared below would be the same order.
  std::vector<PositionId> nativeOrder;
  for (const auto& [pid, pos] : positions.positions())
  {
    nativeOrder.push_back(pid);
  }
  ASSERT_FALSE(std::is_sorted(nativeOrder.begin(), nativeOrder.end()))
      << "unordered_map<PositionId, IndividualPosition> enumerated positions "
         "in ascending id order on this build; pick a data set that actually "
         "exercises hash-bucket order to keep this test meaningful";

  // The reference: fold in explicit ascending PositionId order -- exactly
  // what the fix inside compute() does internally.
  std::vector<PositionId> sortedOrder = nativeOrder;
  std::sort(sortedOrder.begin(), sortedOrder.end());
  double sortedSum = 0.0;
  for (PositionId pid : sortedOrder)
  {
    sortedSum += positions.positions().at(pid).contractMultiplier;
  }

  // What folding in the map's own (native, bucket) order gives -- this is
  // what compute() produced before the fix.
  double nativeSum = 0.0;
  for (PositionId pid : nativeOrder)
  {
    nativeSum += positions.positions().at(pid).contractMultiplier;
  }

  // The data set must actually be order-sensitive for these two positions'
  // ids and this library's bucket layout, or a mutation that removes the
  // sort in compute() could not be caught by the assertion below.
  ASSERT_NE(std::bit_cast<uint64_t>(sortedSum), std::bit_cast<uint64_t>(nativeSum))
      << "native and sorted fold orders happened to agree on this data set "
         "(sortedSum="
      << sortedSum << ", nativeSum=" << nativeSum << ")";

  PortfolioGreeksAggregator agg(reg);
  auto pg = agg.compute(kNow, positions, [](SymbolId)
                        { return 100.0; }, [](SymbolId)
                        { return 0.6; });

  // compute() must match the sorted-order reference bit-for-bit. memcmp via
  // bit_cast rather than EXPECT_DOUBLE_EQ: the bug this guards against is
  // exactly a low-bits divergence that EXPECT_DOUBLE_EQ's ULP tolerance
  // would let through.
  EXPECT_EQ(std::bit_cast<uint64_t>(pg.delta), std::bit_cast<uint64_t>(sortedSum));
}
