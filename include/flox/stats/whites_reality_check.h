/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// White's reality check: bootstrap test for whether the best-performing
// strategy among K candidates is significantly better than zero (or
// against a benchmark; the caller passes excess returns) AFTER
// accounting for the multiple-comparison bias from picking the best.
//
// Reference: White, H. (2000). "A Reality Check for Data Snooping."
//
// Algorithm:
//   1. For each strategy k, compute mean return f_k = mean over t of
//      returns[k][t]. The caller passes excess returns; benchmark
//      adjustment is the caller's responsibility.
//   2. Test statistic V_n = sqrt(T) * max_k f_k.
//   3. Stationary bootstrap (Politis & Romano 1994) with mean block
//      size L. Each bootstrap sample resamples a SHARED sequence of
//      indices for all K strategies, preserving cross-strategy
//      correlation. Inside one block of length geom(1/L), consecutive
//      indices are kept in order, wrapping at T.
//   4. For sample b, compute V_n^b = sqrt(T) * max_k (f_k_b - f_k).
//      Subtracting f_k recentres the bootstrap distribution under the
//      null hypothesis of zero mean.
//   5. p-value = #{V_n^b >= V_n} / B.
//
// avg_block_size <= 0 → auto: sqrt(T) heuristic.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace flox::stats
{

struct WhitesRealityCheckResult
{
  double pValue;
  double bestStat;  // sqrt(T) * max_k f_k
  int bestIndex;    // 0..K-1
};

inline WhitesRealityCheckResult whitesRealityCheck(
    const double* returns,
    std::size_t numStrategies,
    std::size_t numPeriods,
    std::uint32_t numBootstrap = 10000u,
    double avgBlockSize = 0.0,
    std::uint64_t seed = 42u)
{
  if (numStrategies == 0 || numPeriods == 0 || numBootstrap == 0)
  {
    return {1.0, 0.0, -1};
  }

  const std::size_t K = numStrategies;
  const std::size_t T = numPeriods;
  const double sqrtT = std::sqrt(static_cast<double>(T));

  // Per-strategy mean f_k.
  std::vector<double> fMean(K, 0.0);
  for (std::size_t k = 0; k < K; ++k)
  {
    double s = 0.0;
    for (std::size_t t = 0; t < T; ++t)
    {
      s += returns[k * T + t];
    }
    fMean[k] = s / static_cast<double>(T);
  }

  double observedBest = -std::numeric_limits<double>::infinity();
  int bestIndex = 0;
  for (std::size_t k = 0; k < K; ++k)
  {
    if (fMean[k] > observedBest)
    {
      observedBest = fMean[k];
      bestIndex = static_cast<int>(k);
    }
  }
  const double V_n = sqrtT * observedBest;

  // Stationary bootstrap. Mean block length L; per-step probability
  // of starting a new block is q = 1/L.
  if (avgBlockSize <= 0.0)
  {
    avgBlockSize = std::sqrt(static_cast<double>(T));
  }
  if (avgBlockSize < 1.0)
  {
    avgBlockSize = 1.0;
  }
  const double q = 1.0 / avgBlockSize;

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::size_t> uniformIdx(0, T - 1);
  std::uniform_real_distribution<double> uniformReal(0.0, 1.0);

  std::vector<std::size_t> indices(T, 0);
  std::size_t extreme = 0;

  for (std::uint32_t b = 0; b < numBootstrap; ++b)
  {
    std::size_t cur = uniformIdx(rng);
    for (std::size_t t = 0; t < T; ++t)
    {
      if (t == 0 || uniformReal(rng) < q)
      {
        cur = uniformIdx(rng);
      }
      else
      {
        cur = (cur + 1) % T;
      }
      indices[t] = cur;
    }

    double V_b = -std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k < K; ++k)
    {
      double s = 0.0;
      for (std::size_t t = 0; t < T; ++t)
      {
        s += returns[k * T + indices[t]];
      }
      const double bootMean = s / static_cast<double>(T);
      const double recentred = bootMean - fMean[k];
      if (recentred > V_b)
      {
        V_b = recentred;
      }
    }
    const double V_n_b = sqrtT * V_b;
    if (V_n_b >= V_n)
    {
      ++extreme;
    }
  }

  const double pValue = static_cast<double>(extreme) /
                        static_cast<double>(numBootstrap);
  return {pValue, V_n, bestIndex};
}

}  // namespace flox::stats
