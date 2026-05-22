/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/latency_distribution.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

using namespace flox;

namespace
{
double mean(const std::vector<int64_t>& v)
{
  double s = 0.0;
  for (auto x : v)
  {
    s += static_cast<double>(x);
  }
  return s / static_cast<double>(v.size());
}

double quantile(std::vector<int64_t> v, double q)
{
  std::sort(v.begin(), v.end());
  size_t idx = std::min(v.size() - 1,
                        static_cast<size_t>(q * static_cast<double>(v.size())));
  return static_cast<double>(v[idx]);
}

double lag1_autocorr(const std::vector<int64_t>& v)
{
  const double m = mean(v);
  double num = 0.0, den = 0.0;
  for (size_t i = 1; i < v.size(); ++i)
  {
    num += (v[i - 1] - m) * (v[i] - m);
  }
  for (size_t i = 0; i < v.size(); ++i)
  {
    den += (v[i] - m) * (v[i] - m);
  }
  return den > 0.0 ? num / den : 0.0;
}
}  // namespace

TEST(LatencyDistribution, ConstantAlwaysSameValue)
{
  auto d = LatencyDistribution::constant(5'000'000);
  std::mt19937_64 rng(42);
  for (int i = 0; i < 100; ++i)
  {
    EXPECT_EQ(d.sample(rng), 5'000'000);
  }
}

TEST(LatencyDistribution, UniformInBounds)
{
  auto d = LatencyDistribution::uniform(1'000'000, 10'000'000);
  std::mt19937_64 rng(42);
  for (int i = 0; i < 1000; ++i)
  {
    int64_t s = d.sample(rng);
    EXPECT_GE(s, 1'000'000);
    EXPECT_LE(s, 10'000'000);
  }
}

TEST(LatencyDistribution, LognormalHonoursMedianWithin10Percent)
{
  // sigma=0.5, median=5ms. Sample 10k draws; recovered median should
  // sit near the configured value.
  auto d = LatencyDistribution::lognormal(5'000'000, 0.5);
  std::mt19937_64 rng(42);
  std::vector<int64_t> samples;
  samples.reserve(10000);
  for (int i = 0; i < 10000; ++i)
  {
    samples.push_back(d.sample(rng));
  }
  const double recovered = quantile(samples, 0.5);
  EXPECT_NEAR(recovered, 5'000'000.0, 5'000'000.0 * 0.1);
}

TEST(LatencyDistribution, LognormalTailExceedsUniformAtSameMedian)
{
  // Same median, but lognormal p99 should overshoot a uniform [base,
  // 2*base] tail materially.
  std::mt19937_64 rng(42);
  auto uni = LatencyDistribution::uniform(2'000'000, 8'000'000);  // median 5ms
  auto log = LatencyDistribution::lognormal(5'000'000, 0.8);

  std::vector<int64_t> uniS, logS;
  uniS.reserve(10000);
  logS.reserve(10000);
  for (int i = 0; i < 10000; ++i)
  {
    uniS.push_back(uni.sample(rng));
    logS.push_back(log.sample(rng));
  }
  const double uniP99 = quantile(uniS, 0.99);
  const double logP99 = quantile(logS, 0.99);
  EXPECT_GT(logP99, uniP99 * 1.5);
}

TEST(LatencyDistribution, EmpiricalRedrawsFromHistogram)
{
  std::vector<int64_t> samplesIn = {1'000'000, 1'000'000, 1'000'000, 50'000'000};
  auto d = LatencyDistribution::empirical(samplesIn);
  std::mt19937_64 rng(42);
  int big = 0;
  for (int i = 0; i < 1000; ++i)
  {
    if (d.sample(rng) >= 50'000'000)
    {
      ++big;
    }
  }
  // ~250 of 1000 expected; tolerate 100..400.
  EXPECT_GT(big, 100);
  EXPECT_LT(big, 400);
}

TEST(LatencyDistribution, BurstCorrelationProducesAutocorrelation)
{
  auto independent = LatencyDistribution::lognormal(5'000'000, 0.7);
  auto correlated = LatencyDistribution::lognormal(5'000'000, 0.7);
  correlated.setBurstCorrelation(0.9);

  std::mt19937_64 rng(42);
  std::vector<int64_t> indSamples, corSamples;
  indSamples.reserve(2000);
  corSamples.reserve(2000);
  for (int i = 0; i < 2000; ++i)
  {
    indSamples.push_back(independent.sample(rng));
  }
  rng.seed(42);
  for (int i = 0; i < 2000; ++i)
  {
    corSamples.push_back(correlated.sample(rng));
  }
  const double indRho = lag1_autocorr(indSamples);
  const double corRho = lag1_autocorr(corSamples);
  // Independent should be near zero; correlated should be materially higher.
  EXPECT_LT(std::abs(indRho), 0.1);
  EXPECT_GT(corRho, 0.3);
}

TEST(LatencyDistribution, ZeroBurstFallsBackToIndependent)
{
  auto d = LatencyDistribution::lognormal(5'000'000, 0.5);
  d.setBurstCorrelation(0.0);
  std::mt19937_64 rng(42);
  std::vector<int64_t> samples;
  samples.reserve(2000);
  for (int i = 0; i < 2000; ++i)
  {
    samples.push_back(d.sample(rng));
  }
  EXPECT_LT(std::abs(lag1_autocorr(samples)), 0.1);
}

TEST(LatencyDistribution, MedianAccessor)
{
  EXPECT_EQ(LatencyDistribution::constant(123).medianNs(), 123);
  EXPECT_EQ(LatencyDistribution::uniform(100, 300).medianNs(), 200);
  EXPECT_EQ(LatencyDistribution::lognormal(7'000'000, 0.5).medianNs(), 7'000'000);
  EXPECT_EQ(LatencyDistribution::empirical({1, 2, 3, 4, 5}).medianNs(), 3);
}
