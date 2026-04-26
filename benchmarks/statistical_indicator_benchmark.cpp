#include "flox/indicator/correlation.h"
#include "flox/indicator/kurtosis.h"
#include "flox/indicator/parkinson_vol.h"
#include "flox/indicator/rogers_satchell_vol.h"
#include "flox/indicator/rolling_zscore.h"
#include "flox/indicator/shannon_entropy.h"
#include "flox/indicator/skewness.h"

#include <benchmark/benchmark.h>
#include <random>
#include <vector>

using namespace flox::indicator;

static std::vector<double> randomData(size_t n, double base = 100.0)
{
  std::mt19937 rng(42);
  std::normal_distribution<double> dist(0.0, 1.0);
  std::vector<double> data(n);
  data[0] = base;
  for (size_t i = 1; i < n; ++i)
  {
    data[i] = data[i - 1] + dist(rng);
  }
  return data;
}

static void BM_Skewness(benchmark::State& state)
{
  auto data = randomData(state.range(0));
  Skewness ind(20);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(ind.compute(data));
  }
}
BENCHMARK(BM_Skewness)->RangeMultiplier(4)->Range(1024, 131072);

static void BM_Kurtosis(benchmark::State& state)
{
  auto data = randomData(state.range(0));
  Kurtosis ind(20);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(ind.compute(data));
  }
}
BENCHMARK(BM_Kurtosis)->RangeMultiplier(4)->Range(1024, 131072);

static void BM_RollingZScore(benchmark::State& state)
{
  auto data = randomData(state.range(0));
  RollingZScore ind(20);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(ind.compute(data));
  }
}
BENCHMARK(BM_RollingZScore)->RangeMultiplier(4)->Range(1024, 131072);

static void BM_ShannonEntropy(benchmark::State& state)
{
  auto data = randomData(state.range(0));
  ShannonEntropy ind(20, 10);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(ind.compute(data));
  }
}
BENCHMARK(BM_ShannonEntropy)->RangeMultiplier(4)->Range(1024, 131072);

static void BM_ParkinsonVol(benchmark::State& state)
{
  size_t n = state.range(0);
  auto base = randomData(n);
  std::vector<double> high(n), low(n);
  for (size_t i = 0; i < n; ++i)
  {
    high[i] = base[i] + 1.0;
    low[i] = base[i] - 1.0;
  }
  ParkinsonVol ind(20);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(ind.compute(high, low));
  }
}
BENCHMARK(BM_ParkinsonVol)->RangeMultiplier(4)->Range(1024, 131072);

static void BM_RogersSatchellVol(benchmark::State& state)
{
  size_t n = state.range(0);
  auto base = randomData(n);
  std::vector<double> o(n), h(n), l(n), c(n);
  for (size_t i = 0; i < n; ++i)
  {
    o[i] = base[i];
    h[i] = base[i] + 1.5;
    l[i] = base[i] - 1.5;
    c[i] = base[i] + 0.5;
  }
  RogersSatchellVol ind(20);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(ind.compute(o, h, l, c));
  }
}
BENCHMARK(BM_RogersSatchellVol)->RangeMultiplier(4)->Range(1024, 131072);

static void BM_Correlation(benchmark::State& state)
{
  auto x = randomData(state.range(0), 100.0);
  auto y = randomData(state.range(0), 50.0);
  Correlation ind(20);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(ind.compute(x, y));
  }
}
BENCHMARK(BM_Correlation)->RangeMultiplier(4)->Range(1024, 131072);

BENCHMARK_MAIN();
