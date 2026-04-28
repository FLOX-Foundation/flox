#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "flox/target/future_ctc_volatility.h"
#include "flox/target/future_linear_slope.h"
#include "flox/target/future_return.h"

using namespace flox::target;

static std::vector<double> linearSeries(size_t n, double base, double step)
{
  std::vector<double> out(n);
  for (size_t i = 0; i < n; ++i)
  {
    out[i] = base + step * static_cast<double>(i);
  }
  return out;
}

// ── FutureReturn ─────────────────────────────────────────────────────

TEST(FutureReturn, BasicForwardRatio)
{
  std::vector<double> close = {100.0, 101.0, 99.0, 105.0, 110.0};
  FutureReturn fr(2);
  auto out = fr.compute(close);

  ASSERT_EQ(out.size(), close.size());
  EXPECT_NEAR(out[0], 99.0 / 100.0 - 1.0, 1e-12);
  EXPECT_NEAR(out[1], 105.0 / 101.0 - 1.0, 1e-12);
  EXPECT_NEAR(out[2], 110.0 / 99.0 - 1.0, 1e-12);
  EXPECT_TRUE(std::isnan(out[3]));
  EXPECT_TRUE(std::isnan(out[4]));
}

TEST(FutureReturn, TooShortAllNan)
{
  std::vector<double> close = {1.0, 2.0};
  FutureReturn fr(5);
  auto out = fr.compute(close);
  for (double v : out)
  {
    EXPECT_TRUE(std::isnan(v));
  }
}

TEST(FutureReturn, ZeroBaseGivesNan)
{
  std::vector<double> close = {0.0, 1.0, 2.0, 3.0};
  FutureReturn fr(1);
  auto out = fr.compute(close);
  EXPECT_TRUE(std::isnan(out[0]));
  EXPECT_NEAR(out[1], 1.0, 1e-12);
  EXPECT_NEAR(out[2], 0.5, 1e-12);
  EXPECT_TRUE(std::isnan(out[3]));
}

// ── FutureCTCVolatility ──────────────────────────────────────────────

TEST(FutureCTCVolatility, ConstantSeriesIsZero)
{
  std::vector<double> close(20, 100.0);
  FutureCTCVolatility v(5);
  auto out = v.compute(close);
  for (size_t i = 0; i + 5 < close.size(); ++i)
  {
    EXPECT_NEAR(out[i], 0.0, 1e-12);
  }
  for (size_t i = close.size() - 5; i < close.size(); ++i)
  {
    EXPECT_TRUE(std::isnan(out[i]));
  }
}

TEST(FutureCTCVolatility, MatchesSampleStdOfLogReturns)
{
  std::vector<double> close = {100.0, 102.0, 101.0, 105.0, 103.0, 108.0};
  FutureCTCVolatility v(3);
  auto out = v.compute(close);

  // out[0] uses returns over [c0..c3]: ln(102/100), ln(101/102), ln(105/101)
  std::vector<double> r = {std::log(102.0 / 100.0), std::log(101.0 / 102.0),
                           std::log(105.0 / 101.0)};
  double m = (r[0] + r[1] + r[2]) / 3.0;
  double s = 0.0;
  for (double x : r)
  {
    s += (x - m) * (x - m);
  }
  double expected = std::sqrt(s / 2.0);
  EXPECT_NEAR(out[0], expected, 1e-12);

  EXPECT_TRUE(std::isnan(out[3]));
  EXPECT_TRUE(std::isnan(out[4]));
  EXPECT_TRUE(std::isnan(out[5]));
}

TEST(FutureCTCVolatility, NonPositivePriceGivesNan)
{
  std::vector<double> close = {100.0, 0.0, 101.0, 102.0, 103.0};
  FutureCTCVolatility v(2);
  auto out = v.compute(close);
  EXPECT_TRUE(std::isnan(out[0]));
  EXPECT_TRUE(std::isnan(out[1]));
  EXPECT_FALSE(std::isnan(out[2]));
}

// ── FutureLinearSlope ────────────────────────────────────────────────

TEST(FutureLinearSlope, ExactSlopeOnLinearSeries)
{
  auto close = linearSeries(20, 100.0, 0.5);
  FutureLinearSlope s(4);
  auto out = s.compute(close);

  for (size_t t = 0; t + 4 < close.size(); ++t)
  {
    EXPECT_NEAR(out[t], 0.5, 1e-12) << "t=" << t;
  }
  for (size_t t = close.size() - 4; t < close.size(); ++t)
  {
    EXPECT_TRUE(std::isnan(out[t]));
  }
}

TEST(FutureLinearSlope, NegativeSlope)
{
  auto close = linearSeries(10, 100.0, -2.5);
  FutureLinearSlope s(3);
  auto out = s.compute(close);
  for (size_t t = 0; t + 3 < close.size(); ++t)
  {
    EXPECT_NEAR(out[t], -2.5, 1e-12);
  }
}

TEST(FutureLinearSlope, MatchesClosedFormOLS)
{
  // y = 0.7 * x + 5 plus a bump
  std::vector<double> close = {5.0, 6.0, 7.5, 7.0, 8.5, 9.0, 10.5};
  FutureLinearSlope s(4);
  auto out = s.compute(close);

  // For t = 0, fit y over x = 0..4 to {5.0, 6.0, 7.5, 7.0, 8.5}.
  // closed-form OLS slope:
  // sumX = 10, sumX2 = 30, n = 5, denom = 5*30 - 10*10 = 50
  // sumY = 34.0, sumXY = 0*5 + 1*6 + 2*7.5 + 3*7 + 4*8.5 = 0+6+15+21+34 = 76
  // slope = (5*76 - 10*34) / 50 = (380 - 340) / 50 = 0.8
  EXPECT_NEAR(out[0], 0.8, 1e-12);
}

TEST(FutureLinearSlope, HorizonOneIsForwardDifference)
{
  std::vector<double> close = {100.0, 101.5, 99.0, 105.0};
  FutureLinearSlope s(1);
  auto out = s.compute(close);
  EXPECT_NEAR(out[0], 1.5, 1e-12);
  EXPECT_NEAR(out[1], -2.5, 1e-12);
  EXPECT_NEAR(out[2], 6.0, 1e-12);
  EXPECT_TRUE(std::isnan(out[3]));
}
