#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

#include "flox/indicator/adf.h"

using namespace flox::indicator;

namespace
{

std::vector<double> randomWalk(size_t n, uint32_t seed = 42, double sigma = 1.0)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> nd(0.0, sigma);
  std::vector<double> y(n);
  y[0] = 0.0;
  for (size_t i = 1; i < n; ++i)
  {
    y[i] = y[i - 1] + nd(rng);
  }
  return y;
}

std::vector<double> stationaryAR1(size_t n, double phi, uint32_t seed = 42, double sigma = 1.0)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> nd(0.0, sigma);
  std::vector<double> y(n);
  y[0] = nd(rng);
  for (size_t i = 1; i < n; ++i)
  {
    y[i] = phi * y[i - 1] + nd(rng);
  }
  return y;
}

}  // namespace

TEST(ADF, RandomWalkIsNonStationary)
{
  auto y = randomWalk(500);
  auto r = adf(y, 4, AdfRegression::Constant);
  // Random walk: cannot reject the unit-root null at 5%.
  EXPECT_GT(r.test_stat, -2.861);
  EXPECT_GT(r.p_value, 0.05);
  EXPECT_LE(r.used_lag, 4u);
}

TEST(ADF, StationaryAR1IsRejected)
{
  // φ = 0.3 is well inside the unit circle; large sample → easy to reject.
  auto y = stationaryAR1(500, 0.3);
  auto r = adf(y, 4, AdfRegression::Constant);
  EXPECT_LT(r.test_stat, -2.861) << "expected rejection at 5% level, got " << r.test_stat;
  EXPECT_LT(r.p_value, 0.05);
}

TEST(ADF, RegressionStringParse)
{
  auto y = randomWalk(200);
  auto rn = adf(y, 2, std::string("n"));
  auto rc = adf(y, 2, std::string("c"));
  auto rt = adf(y, 2, std::string("ct"));
  EXPECT_TRUE(std::isfinite(rn.test_stat));
  EXPECT_TRUE(std::isfinite(rc.test_stat));
  EXPECT_TRUE(std::isfinite(rt.test_stat));
}

TEST(ADF, InvalidRegressionThrows)
{
  std::vector<double> y(20, 1.0);
  EXPECT_THROW(adf(y, 1, std::string("xx")), std::invalid_argument);
}

TEST(ADF, NaNInputThrows)
{
  std::vector<double> y = {1.0, 2.0, std::nan(""), 4.0, 5.0};
  EXPECT_THROW(adf(y, 1), std::invalid_argument);
}

TEST(ADF, TooShortInputThrows)
{
  std::vector<double> y = {1.0, 2.0, 3.0};
  EXPECT_THROW(adf(y, 0), std::invalid_argument);
}

TEST(ADF, ZeroLagWorks)
{
  auto y = stationaryAR1(200, 0.5);
  auto r = adf(y, 0, AdfRegression::Constant);
  EXPECT_EQ(r.used_lag, 0u);
  EXPECT_TRUE(std::isfinite(r.test_stat));
}

TEST(ADF, PValueMonotoneInTestStat)
{
  // Ensure p-value monotonicity by feeding two series with very different
  // strengths of mean reversion.
  auto rwalk = randomWalk(400);
  auto stationary = stationaryAR1(400, 0.1);

  auto rWalk = adf(rwalk, 3, AdfRegression::Constant);
  auto rStat = adf(stationary, 3, AdfRegression::Constant);

  EXPECT_LT(rStat.test_stat, rWalk.test_stat);
  EXPECT_LE(rStat.p_value, rWalk.p_value);
}
