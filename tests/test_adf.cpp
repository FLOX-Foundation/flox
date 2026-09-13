#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

#include "flox/error/flox_error.h"
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
  EXPECT_THROW(adf(y, 1, std::string("xx")), flox::FloxError);
}

TEST(ADF, NaNInputThrows)
{
  std::vector<double> y = {1.0, 2.0, std::nan(""), 4.0, 5.0};
  EXPECT_THROW(adf(y, 1), flox::FloxError);
}

TEST(ADF, TooShortInputThrows)
{
  std::vector<double> y = {1.0, 2.0, 3.0};
  EXPECT_THROW(adf(y, 0), flox::FloxError);
}

TEST(ADF, ZeroLagWorks)
{
  auto y = stationaryAR1(200, 0.5);
  auto r = adf(y, 0, AdfRegression::Constant);
  EXPECT_EQ(r.used_lag, 0u);
  EXPECT_TRUE(std::isfinite(r.test_stat));
}

// The AIC-based lag search used to compare candidates on a floating sample
// size (n = T - lag - 1, different for every candidate lag), so rescaling the
// series -- dollars to cents, price to log-price, BTC-sized values to
// ETH/BTC-sized ones -- shifted every candidate's AIC by an amount that
// depends on that candidate's lag and flipped which lag AIC preferred. The
// regression itself is scale-invariant (beta and its SE both scale linearly,
// so tau cancels the scale exactly); only the lag *selection* wasn't. Fixing
// the sample to T - max_lag - 1 for every candidate (matching
// statsmodels' adfuller(autolag="AIC")) makes the selected lag, and therefore
// the reported statistic, invariant to a pure rescaling of the input.
TEST(ADF, ScaleInvariantLagSelection)
{
  auto y = stationaryAR1(300, 0.95);
  std::vector<double> scaled(y.size());
  for (size_t i = 0; i < y.size(); ++i)
  {
    scaled[i] = y[i] * 1000.0;
  }

  auto r1 = adf(y, 10, AdfRegression::Constant);
  auto r2 = adf(scaled, 10, AdfRegression::Constant);

  EXPECT_EQ(r1.used_lag, r2.used_lag);
  EXPECT_NEAR(r1.test_stat, r2.test_stat, 1e-6);
  EXPECT_NEAR(r1.p_value, r2.p_value, 1e-9);
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
