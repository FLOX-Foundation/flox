#include <gtest/gtest.h>

#include "flox/pricing/vol_cone.h"

#include <cmath>
#include <random>
#include <vector>

using namespace flox::pricing;

namespace
{
// A geometric price path whose daily log returns are Normal(0, dailyVol).
std::vector<double> syntheticPath(double dailyVol, int n, unsigned seed)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> nd(0.0, dailyVol);
  std::vector<double> px;
  px.reserve(n);
  double p = 100.0;
  px.push_back(p);
  for (int i = 1; i < n; ++i)
  {
    p *= std::exp(nd(rng));
    px.push_back(p);
  }
  return px;
}
}  // namespace

// Realized vol of a window recovers the annualized vol of the generating
// process: daily vol 0.02 over 252 trading days ~ 0.02 * sqrt(252) ~ 0.317.
TEST(VolConeTest, RealizedVolRecoversAnnualized)
{
  const auto px = syntheticPath(0.02, 4000, 42);
  const auto rets = logReturns(px);
  const double rv = realizedVol(rets, 252.0);
  EXPECT_NEAR(rv, 0.02 * std::sqrt(252.0), 0.02);
}

// A higher-vol process produces a cone sitting above a lower-vol one at the same
// horizon.
TEST(VolConeTest, HigherVolLiftsTheCone)
{
  const auto calm = syntheticPath(0.01, 2000, 1);
  const auto wild = syntheticPath(0.04, 2000, 1);
  const std::vector<size_t> horizons = {20};

  const auto coneCalm = volCone(calm, horizons, 252.0);
  const auto coneWild = volCone(wild, horizons, 252.0);
  ASSERT_EQ(coneCalm.size(), 1u);
  ASSERT_EQ(coneWild.size(), 1u);
  EXPECT_LT(coneCalm[0].p50, coneWild[0].p50);
  EXPECT_LT(coneCalm[0].max, coneWild[0].p50);  // calm's worst < wild's median
}

// Percentiles are ordered min <= p25 <= p50 <= p75 <= max, and the cone reports
// the expected number of rolling windows per horizon.
TEST(VolConeTest, PercentilesOrderedAndCounted)
{
  const auto px = syntheticPath(0.02, 500, 7);  // 499 returns
  const std::vector<size_t> horizons = {10, 30, 90};
  const auto cone = volCone(px, horizons, 252.0);

  ASSERT_EQ(cone.size(), 3u);
  for (const auto& c : cone)
  {
    EXPECT_LE(c.min, c.p25);
    EXPECT_LE(c.p25, c.p50);
    EXPECT_LE(c.p50, c.p75);
    EXPECT_LE(c.p75, c.max);
    EXPECT_EQ(c.samples, 499u - c.horizon + 1u);  // rolling windows over 499 returns
  }
}

// Implied placed in the cone: above every realized sample -> ~1, below -> 0,
// and a value at the median lands near 0.5.
TEST(VolConeTest, ImpliedPercentileInCone)
{
  const auto px = syntheticPath(0.02, 1500, 11);
  const auto rv = rollingRealizedVols(logReturns(px), 30, 252.0);

  ASSERT_FALSE(rv.empty());
  std::vector<double> sorted = rv;
  std::sort(sorted.begin(), sorted.end());
  const double median = sorted[sorted.size() / 2];

  EXPECT_DOUBLE_EQ(impliedPercentileInCone(rv, sorted.back() + 1.0), 1.0);
  EXPECT_DOUBLE_EQ(impliedPercentileInCone(rv, sorted.front() - 1.0), 0.0);
  EXPECT_NEAR(impliedPercentileInCone(rv, median), 0.5, 0.05);
}

// Short series degrade gracefully: a horizon longer than the history yields zero
// samples, and an empty cone returns NaN for the implied percentile.
TEST(VolConeTest, ShortSeriesGraceful)
{
  const std::vector<double> px = {100.0, 101.0, 102.0};  // 2 returns
  const auto cone = volCone(px, {10}, 252.0);
  ASSERT_EQ(cone.size(), 1u);
  EXPECT_EQ(cone[0].samples, 0u);
  EXPECT_TRUE(std::isnan(impliedPercentileInCone({}, 0.3)));
}
