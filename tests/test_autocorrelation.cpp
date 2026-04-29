#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

#include "flox/indicator/autocorrelation.h"
#include "flox/indicator/correlation.h"

using namespace flox::indicator;

TEST(AutoCorrelation, MatchesCorrelationOnPairedSeries)
{
  // Reference: build (x[lag..], x[..-lag]) explicitly and run Correlation.
  std::mt19937 rng(123);
  std::uniform_real_distribution<double> u(-1.0, 1.0);
  std::vector<double> x(50);
  for (auto& v : x)
  {
    v = u(rng);
  }

  const size_t window = 10;
  const size_t lag = 3;

  auto autoOut = AutoCorrelation(window, lag).compute(x);

  std::vector<double> a(x.begin() + lag, x.end());
  std::vector<double> b(x.begin(), x.end() - lag);
  auto refOut = Correlation(window).compute(a, b);

  // AutoCorrelation reports at index (t in original frame) = window+lag-1+i;
  // Correlation on the paired arrays reports at index (window-1+i).
  for (size_t i = 0; i + window - 1 < a.size(); ++i)
  {
    size_t origIdx = window + lag - 1 + i;
    size_t pairedIdx = window - 1 + i;
    if (std::isnan(refOut[pairedIdx]))
    {
      EXPECT_TRUE(std::isnan(autoOut[origIdx]));
    }
    else
    {
      EXPECT_NEAR(autoOut[origIdx], refOut[pairedIdx], 1e-12);
    }
  }
}

TEST(AutoCorrelation, WarmupNaN)
{
  std::vector<double> x(20, 1.0);
  auto out = AutoCorrelation(5, 2).compute(x);
  // First valid index is window+lag-1 = 6.
  for (size_t i = 0; i < 6; ++i)
  {
    EXPECT_TRUE(std::isnan(out[i]));
  }
}

TEST(AutoCorrelation, ConstantSeriesIsNan)
{
  // Zero variance → undefined correlation.
  std::vector<double> x(30, 1.0);
  auto out = AutoCorrelation(5, 1).compute(x);
  for (size_t i = 5; i < x.size(); ++i)
  {
    EXPECT_TRUE(std::isnan(out[i])) << "i=" << i;
  }
}

TEST(AutoCorrelation, LinearSeriesLag1IsOne)
{
  // For y = a + b*t, lag-1 autocorrelation over any window is exactly 1
  // (perfect linear pairing).
  std::vector<double> x(50);
  for (size_t i = 0; i < x.size(); ++i)
  {
    x[i] = 5.0 + 0.7 * static_cast<double>(i);
  }
  auto out = AutoCorrelation(10, 1).compute(x);
  for (size_t i = 10; i < x.size(); ++i)
  {
    EXPECT_NEAR(out[i], 1.0, 1e-10) << "i=" << i;
  }
}

TEST(AutoCorrelation, FreeFunctionMatchesClass)
{
  std::vector<double> x = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  auto a = AutoCorrelation(3, 1).compute(x);
  auto b = autocorrelation(x, 3, 1);
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (std::isnan(a[i]))
    {
      EXPECT_TRUE(std::isnan(b[i]));
    }
    else
    {
      EXPECT_DOUBLE_EQ(a[i], b[i]);
    }
  }
}
