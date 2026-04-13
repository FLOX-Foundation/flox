// python/optimizer_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

namespace py = pybind11;

namespace
{

// Permutation test: tests whether two groups have the same mean
// Returns p-value (probability of observing the actual difference by chance)
double doPermutationTest(const double* g1, size_t n1, const double* g2, size_t n2,
                         size_t numPermutations)
{
  double sum1 = 0, sum2 = 0;
  for (size_t i = 0; i < n1; ++i)
  {
    sum1 += g1[i];
  }
  for (size_t i = 0; i < n2; ++i)
  {
    sum2 += g2[i];
  }
  double observedDiff = std::abs(sum1 / n1 - sum2 / n2);

  // Pool all values
  std::vector<double> pool(n1 + n2);
  std::copy(g1, g1 + n1, pool.begin());
  std::copy(g2, g2 + n2, pool.begin() + n1);

  std::mt19937 rng(42);
  size_t extremeCount = 0;

  for (size_t p = 0; p < numPermutations; ++p)
  {
    std::shuffle(pool.begin(), pool.end(), rng);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n1; ++i)
    {
      s1 += pool[i];
    }
    for (size_t i = n1; i < n1 + n2; ++i)
    {
      s2 += pool[i];
    }
    double diff = std::abs(s1 / n1 - s2 / n2);
    if (diff >= observedDiff)
    {
      ++extremeCount;
    }
  }

  return static_cast<double>(extremeCount) / numPermutations;
}

// Pearson correlation coefficient
double doCorrelation(const double* x, const double* y, size_t n)
{
  double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0, sumY2 = 0;
  for (size_t i = 0; i < n; ++i)
  {
    sumX += x[i];
    sumY += y[i];
    sumXY += x[i] * y[i];
    sumX2 += x[i] * x[i];
    sumY2 += y[i] * y[i];
  }

  double nd = static_cast<double>(n);
  double num = nd * sumXY - sumX * sumY;
  double den = std::sqrt((nd * sumX2 - sumX * sumX) * (nd * sumY2 - sumY * sumY));

  if (den == 0.0)
  {
    return 0.0;
  }
  return num / den;
}

// Bootstrap confidence interval
// Returns (lower, median, upper)
std::tuple<double, double, double> doBootstrapCI(const double* data, size_t n,
                                                 double confidence, size_t numSamples)
{
  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, n - 1);

  std::vector<double> means(numSamples);
  for (size_t s = 0; s < numSamples; ++s)
  {
    double sum = 0;
    for (size_t i = 0; i < n; ++i)
    {
      sum += data[dist(rng)];
    }
    means[s] = sum / n;
  }

  std::sort(means.begin(), means.end());

  double alpha = (1.0 - confidence) / 2.0;
  size_t lowerIdx = static_cast<size_t>(alpha * numSamples);
  size_t upperIdx = static_cast<size_t>((1.0 - alpha) * numSamples);
  size_t medianIdx = numSamples / 2;

  if (lowerIdx >= numSamples)
  {
    lowerIdx = 0;
  }
  if (upperIdx >= numSamples)
  {
    upperIdx = numSamples - 1;
  }

  return {means[lowerIdx], means[medianIdx], means[upperIdx]};
}

}  // namespace

inline void bindOptimizer(py::module_& m)
{
  m.def(
      "permutation_test",
      [](py::array_t<double> group1, py::array_t<double> group2, size_t numPermutations)
      {
        auto* g1 = group1.data();
        auto* g2 = group2.data();
        size_t n1 = group1.size();
        size_t n2 = group2.size();

        double result;
        {
          py::gil_scoped_release release;
          result = doPermutationTest(g1, n1, g2, n2, numPermutations);
        }
        return result;
      },
      "Two-sample permutation test, returns p-value",
      py::arg("group1"), py::arg("group2"), py::arg("num_permutations") = 10000);

  m.def(
      "correlation",
      [](py::array_t<double> x, py::array_t<double> y)
      {
        if (x.size() != y.size())
        {
          throw std::invalid_argument("x and y must have the same length");
        }
        auto* xp = x.data();
        auto* yp = y.data();
        size_t n = x.size();

        double result;
        {
          py::gil_scoped_release release;
          result = doCorrelation(xp, yp, n);
        }
        return result;
      },
      "Pearson correlation coefficient",
      py::arg("x"), py::arg("y"));

  m.def(
      "bootstrap_ci",
      [](py::array_t<double> data, double confidence, size_t numSamples)
      {
        auto* dp = data.data();
        size_t n = data.size();
        if (n == 0)
        {
          throw std::invalid_argument("data must not be empty");
        }

        std::tuple<double, double, double> result;
        {
          py::gil_scoped_release release;
          result = doBootstrapCI(dp, n, confidence, numSamples);
        }
        return py::make_tuple(std::get<0>(result), std::get<1>(result), std::get<2>(result));
      },
      "Bootstrap confidence interval, returns (lower, median, upper)",
      py::arg("data"), py::arg("confidence") = 0.95, py::arg("num_samples") = 10000);
}
