#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <random>
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

// Tries a handful of concrete key-insertion layouts for the same logical
// value set and returns them in the order they should be tried. Different
// standard libraries lay std::unordered_map<PositionId, ...> out
// completely differently for a small, dense integer key range: libc++ and
// libstdc++ scramble it by hash bucket, while MSVC STL keeps it close to
// insertion order. Spreading the real ids apart with spacer entries (never
// contributing to any sum, since their value is 0.0) gives each attempt a
// genuinely different absolute key range to hash, without changing which
// logical values are being folded.
struct KeyLayoutAttempt
{
  const char* name;
  std::vector<int> gapsBeforeEach;  // spacer count opened before each real position
};

std::vector<KeyLayoutAttempt> keyLayoutAttempts(size_t n)
{
  std::vector<int> noGaps(n, 0);

  std::vector<int> bigLeadingGap(n, 0);
  bigLeadingGap[0] = 503;

  std::vector<KeyLayoutAttempt> attempts = {
      {"ascending (contiguous ids)", noGaps},
      {"descending-like (large leading gap)", bigLeadingGap},
  };

  // Several independent fixed-seed shuffles rather than one: a single
  // layout can enumerate in non-ascending order while still happening to
  // fold to the same bit pattern as sorted order (seen on windows-clang-cl
  // for the first seed tried). More seeds make it very
  // unlikely every one of them coincides.
  static constexpr unsigned kSeeds[] = {12345u, 67890u, 24680u, 13579u, 99999u};
  static const char* kNames[] = {
      "shuffled (seed 12345)",
      "shuffled (seed 67890)",
      "shuffled (seed 24680)",
      "shuffled (seed 13579)",
      "shuffled (seed 99999)",
  };
  for (size_t s = 0; s < std::size(kSeeds); ++s)
  {
    std::mt19937 rng(kSeeds[s]);
    std::uniform_int_distribution<int> gapDist(1, 7);
    std::vector<int> shuffledGaps(n);
    for (auto& g : shuffledGaps)
    {
      g = gapDist(rng);
    }
    attempts.push_back({kNames[s], std::move(shuffledGaps)});
  }

  return attempts;
}
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

// compute() used to fold delta across positions.positions() in
// whatever order std::unordered_map<PositionId, IndividualPosition> happened
// to enumerate them, which is a hash-bucket order -- a property of the
// standard library, not of the data. libc++ and libstdc++ walk the same
// positions differently, so the aggregate delta below diverged in its low
// bits between machines built against different libraries, breaking
// bit-for-bit backtest reproduction. The fix collects the position ids and
// sorts them before folding.
//
// A single fixed key layout is not enough to exercise this on every
// library: libc++/libstdc++ scramble a small dense integer key range by
// hash bucket, but MSVC STL keeps such a range close to insertion order, so
// the plain "open 16 positions in a row" layout used here previously came
// back already sorted on windows-clang-cl and the test's own self-check
// (correctly) refused to claim it was testing anything. keyLayoutAttempts()
// tries a few different concrete key layouts for the exact same 16 delta
// values -- spacer positions with contractMultiplier 0.0 spread the real
// ids' absolute key values apart without contributing anything to the sum
// (0.0 folds into a running total exactly, regardless of position) -- and
// the test uses the first layout whose real ids do not enumerate in
// ascending order on this build.
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
  // order almost always rounds differently somewhere along the way. Still
  // checked below rather than assumed: a non-ascending native order does
  // not by itself guarantee the fold actually differs.
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
  double sortedSum = 0.0;
  double nativeSum = 0.0;
  bool found = false;

  for (const auto& attempt : keyLayoutAttempts(contributions.size()))
  {
    PositionGroupTracker trial;
    OrderId nextOrderId = 1;
    std::vector<PositionId> realIds;
    for (size_t i = 0; i < contributions.size(); ++i)
    {
      for (int g = 0; g < attempt.gapsBeforeEach[i]; ++g)
      {
        // Spacer: stays open (never closed), but contractMultiplier 0.0
        // means it never changes the running delta sum, wherever it falls
        // in the fold order.
        trial.openPosition(nextOrderId++, perp, Side::BUY, Price::fromDouble(0.0),
                           Quantity::fromDouble(1.0), 0.0);
      }
      PositionId pid = trial.openPosition(nextOrderId++, perp, Side::BUY, Price::fromDouble(0.0),
                                          Quantity::fromDouble(1.0), contributions[i]);
      realIds.push_back(pid);
    }

    std::vector<PositionId> realIdSet = realIds;
    std::sort(realIdSet.begin(), realIdSet.end());

    std::vector<PositionId> nativeOrderReal;
    for (const auto& [pid, pos] : trial.positions())
    {
      if (std::binary_search(realIdSet.begin(), realIdSet.end(), pid))
      {
        nativeOrderReal.push_back(pid);
      }
    }
    if (std::is_sorted(nativeOrderReal.begin(), nativeOrderReal.end()))
    {
      continue;  // this layout enumerated the real ids in ascending order
    }

    // Fold in the map's own native (bucket) order vs. explicit ascending
    // PositionId order -- exactly what compute() did before the fix vs.
    // what it does now. A non-ascending native order is necessary but not
    // sufficient for the two folds to actually disagree (they can still
    // land on the same bit pattern by coincidence), so this attempt is
    // only accepted once both differ.
    std::vector<PositionId> nativeOrder;
    for (const auto& [pid, pos] : trial.positions())
    {
      nativeOrder.push_back(pid);
    }
    std::vector<PositionId> sortedOrder = nativeOrder;
    std::sort(sortedOrder.begin(), sortedOrder.end());

    double trialSortedSum = 0.0;
    for (PositionId pid : sortedOrder)
    {
      trialSortedSum += trial.positions().at(pid).contractMultiplier;
    }
    double trialNativeSum = 0.0;
    for (PositionId pid : nativeOrder)
    {
      trialNativeSum += trial.positions().at(pid).contractMultiplier;
    }

    if (std::bit_cast<uint64_t>(trialSortedSum) == std::bit_cast<uint64_t>(trialNativeSum))
    {
      continue;  // native and sorted folds happened to agree; try another layout
    }

    positions = std::move(trial);
    sortedSum = trialSortedSum;
    nativeSum = trialNativeSum;
    found = true;
    break;
  }

  if (!found)
  {
    GTEST_SKIP() << "no insertion pattern tried produced a native bucket "
                    "order whose fold actually disagrees with the sorted "
                    "one on this standard library; cannot exercise "
                    "bucket-order sensitivity here";
  }

  // The data set was order-sensitive for the chosen layout (checked above,
  // not assumed) -- this is what makes the assertion below meaningful: a
  // mutation that removes the sort in compute() could not otherwise be
  // caught.
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
