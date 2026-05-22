/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace flox
{

// Latency draw distribution for ack-latency knobs (submit / cancel /
// replace). Built-in kinds:
//
//   Constant   — fixed N nanoseconds. Sample is always N.
//   Uniform    — uniform over [lo, hi] nanoseconds. The legacy
//                base+jitter knob lands here (lo = base-jitter,
//                hi = base+jitter).
//   Lognormal  — heavy-tailed, parameterised by median and sigma.
//                exp(mu + sigma * Z) with mu = ln(median).
//   Empirical  — resample with replacement from a recorded
//                histogram of ack times.
//
// Burst correlation models the real-venue observation that when the
// exchange is slow EVERY ack is slow, not just each one independently.
// Implemented as an AR(1) shock on the standard-normal residual (for
// Lognormal) or on the rank index (for Empirical). rho=0 is the
// independent-draw default; rho near 1 produces long runs of slow or
// fast acks.
class LatencyDistribution
{
 public:
  enum class Kind : uint8_t
  {
    Constant = 0,
    Uniform = 1,
    Lognormal = 2,
    Empirical = 3,
  };

  LatencyDistribution() = default;

  static LatencyDistribution constant(int64_t ns)
  {
    LatencyDistribution d;
    d._kind = Kind::Constant;
    d._base = ns;
    return d;
  }

  static LatencyDistribution uniform(int64_t loNs, int64_t hiNs)
  {
    LatencyDistribution d;
    d._kind = Kind::Uniform;
    if (hiNs < loNs)
    {
      std::swap(loNs, hiNs);
    }
    d._base = loNs;
    d._extent = hiNs;
    return d;
  }

  static LatencyDistribution lognormal(int64_t medianNs, double sigma)
  {
    LatencyDistribution d;
    d._kind = Kind::Lognormal;
    d._base = std::max<int64_t>(1, medianNs);
    d._sigma = std::max(0.0, sigma);
    return d;
  }

  static LatencyDistribution empirical(const std::vector<int64_t>& samplesNs)
  {
    LatencyDistribution d;
    d._kind = Kind::Empirical;
    d._samples.reserve(samplesNs.size());
    for (int64_t s : samplesNs)
    {
      d._samples.push_back(std::max<int64_t>(0, s));
    }
    return d;
  }

  // [0..1]. Higher = stronger autocorrelation between consecutive
  // draws. 0 disables (default).
  void setBurstCorrelation(double rho) noexcept
  {
    _rho = std::clamp(rho, 0.0, 0.999);
  }

  double burstCorrelation() const noexcept { return _rho; }
  Kind kind() const noexcept { return _kind; }

  // Median latency for introspection. Constant: the value. Uniform:
  // midpoint. Lognormal: the configured median. Empirical: sorted
  // middle element (sampled, not cached).
  int64_t medianNs() const
  {
    switch (_kind)
    {
      case Kind::Constant:
        return _base;
      case Kind::Uniform:
        return (_base + _extent) / 2;
      case Kind::Lognormal:
        return _base;
      case Kind::Empirical:
      {
        if (_samples.empty())
        {
          return 0;
        }
        auto copy = _samples;
        std::nth_element(copy.begin(), copy.begin() + copy.size() / 2, copy.end());
        return copy[copy.size() / 2];
      }
    }
    return 0;
  }

  // Returns the count of samples held when empirical, 0 otherwise.
  size_t empiricalSampleCount() const noexcept { return _samples.size(); }

  // Sample the next latency in nanoseconds. Updates internal AR(1)
  // state if burst correlation is non-zero. Always returns >= 0.
  int64_t sample(std::mt19937_64& rng)
  {
    switch (_kind)
    {
      case Kind::Constant:
        return _base;

      case Kind::Uniform:
      {
        if (_extent == _base)
        {
          return _base;
        }
        std::uniform_int_distribution<int64_t> dist(_base, _extent);
        const int64_t draw = dist(rng);
        if (_rho > 0.0)
        {
          // AR(1) on normalised position within range.
          double zNew = 2.0 * (static_cast<double>(draw - _base) /
                               static_cast<double>(_extent - _base)) -
                        1.0;
          double zMix = _rho * _lastShock + std::sqrt(1.0 - _rho * _rho) * zNew;
          _lastShock = zMix;
          double pos = (zMix + 1.0) * 0.5;
          pos = std::clamp(pos, 0.0, 1.0);
          return static_cast<int64_t>(_base + pos * (_extent - _base));
        }
        return draw;
      }

      case Kind::Lognormal:
      {
        std::normal_distribution<double> dist(0.0, 1.0);
        double z = dist(rng);
        if (_rho > 0.0)
        {
          z = _rho * _lastShock + std::sqrt(1.0 - _rho * _rho) * z;
          _lastShock = z;
        }
        const double mu = std::log(static_cast<double>(_base));
        const double value = std::exp(mu + _sigma * z);
        return static_cast<int64_t>(std::max(0.0, value));
      }

      case Kind::Empirical:
      {
        if (_samples.empty())
        {
          return 0;
        }
        std::uniform_int_distribution<size_t> dist(0, _samples.size() - 1);
        size_t idx = dist(rng);
        if (_rho > 0.0)
        {
          // Bias the index toward the last picked one for burst.
          const double lastNorm =
              static_cast<double>(_lastIdx) /
              static_cast<double>(std::max<size_t>(1, _samples.size() - 1));
          const double newNorm = static_cast<double>(idx) /
                                 static_cast<double>(std::max<size_t>(1, _samples.size() - 1));
          const double mixed = _rho * lastNorm + (1.0 - _rho) * newNorm;
          idx = static_cast<size_t>(mixed * (_samples.size() - 1));
        }
        _lastIdx = idx;
        return _samples[idx];
      }
    }
    return 0;
  }

 private:
  Kind _kind{Kind::Constant};
  int64_t _base{0};    // Constant ns / Uniform lo / Lognormal median
  int64_t _extent{0};  // Uniform hi
  double _sigma{0.0};  // Lognormal sigma
  std::vector<int64_t> _samples;
  double _rho{0.0};
  double _lastShock{0.0};
  size_t _lastIdx{0};
};

}  // namespace flox
