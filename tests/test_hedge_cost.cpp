#include <gtest/gtest.h>

#include "flox/position/hedge_cost.h"

#include <vector>

using namespace flox;

TEST(HedgeCostTest, DecomposesIntoComponents)
{
  HedgeCostAccumulator acc;
  acc.accrueTheta(-50.0, 1.0 / 365.0);  // long book, -theta is a running cost
  acc.accrueFunding(12.0);
  acc.accrueTransaction(/*units=*/2.0, /*price=*/30000.0, /*costBps=*/5.0);

  const auto& c = acc.cost();
  EXPECT_NEAR(c.thetaCost, 50.0 / 365.0, 1e-9);
  EXPECT_DOUBLE_EQ(c.fundingCost, 12.0);
  EXPECT_DOUBLE_EQ(c.transactionCost, 2.0 * 30000.0 * 5.0 * 1e-4);  // = 30.0
  EXPECT_NEAR(c.total(), c.thetaCost + c.fundingCost + c.transactionCost, 1e-12);
}

TEST(HedgeCostTest, LongBookThetaIsPositiveCost)
{
  HedgeCostAccumulator acc;
  acc.accrueTheta(-100.0, 0.5);  // negative theta -> positive running cost
  EXPECT_GT(acc.cost().thetaCost, 0.0);
}

namespace
{
// Option-book delta path: a drifting delta on a moving mark. Reused across band tests.
std::vector<HedgePathStep> makePath()
{
  return {
      {0.10, 100.0},
      {0.25, 101.0},
      {0.40, 100.5},
      {0.55, 102.0},
      {0.30, 101.5},
      {0.45, 103.0},
      {0.20, 102.5},
      {0.50, 104.0},
  };
}
}  // namespace

TEST(HedgeCostTest, TighterBandCostsMoreTradesLessTrackingError)
{
  const auto path = makePath();
  const double costBps = 5.0;

  const BandResult tight = evaluateBand(0.05, path, costBps);
  const BandResult wide = evaluateBand(0.50, path, costBps);

  EXPECT_GT(tight.rebalances, wide.rebalances);
  EXPECT_GT(tight.transactionCost, wide.transactionCost);
  EXPECT_LT(tight.trackingError, wide.trackingError);
}

TEST(HedgeCostTest, OptimizerPicksMinTotalBand)
{
  const auto path = makePath();
  const std::vector<double> bands = {0.02, 0.05, 0.10, 0.20, 0.35, 0.50, 0.75};
  const auto results = sweepBands(bands, path, /*costBps=*/5.0);

  const double best = bestBand(results);

  double minTotal = std::numeric_limits<double>::infinity();
  double minBand = 0.0;
  for (const auto& r : results)
  {
    if (r.total() < minTotal)
    {
      minTotal = r.total();
      minBand = r.band;
    }
  }
  EXPECT_DOUBLE_EQ(best, minBand);

  // The picked band's total is <= every candidate's total.
  for (const auto& r : results)
  {
    EXPECT_LE(minTotal, r.total() + 1e-12);
  }
}

TEST(HedgeCostTest, EmptyPathIsZeroCostNaNBest)
{
  const auto results = sweepBands({}, {}, 5.0);
  EXPECT_TRUE(results.empty());
  EXPECT_TRUE(std::isnan(bestBand(results)));
}
