/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace flox
{

// One draw from a LatencyModel covering the three latency components
// commonly seen in live trading: feed (event arrival -> engine), order
// (engine submit -> exchange), and fill (exchange match -> engine
// notification). All values are non-negative nanoseconds.
struct LatencySample
{
  int64_t feed_ns{0};
  int64_t order_ns{0};
  int64_t fill_ns{0};
};

// Abstract sampler. Subclasses implement the three component delays.
// sample() composes them. reset() re-seeds the underlying RNG (no-op
// for deterministic models).
class LatencyModel
{
 public:
  virtual ~LatencyModel() = default;

  virtual int64_t feedDelay() = 0;
  virtual int64_t orderDelay() = 0;
  virtual int64_t fillDelay() = 0;

  LatencySample sample()
  {
    LatencySample s;
    s.feed_ns = feedDelay();
    s.order_ns = orderDelay();
    s.fill_ns = fillDelay();
    return s;
  }

  // Override only when the model owns an RNG. Default is a no-op so
  // ConstantLatency does not need to override it.
  virtual void reset(uint64_t /*seed*/) {}
};

// Deterministic baseline. Returns the same nanoseconds every call.
class ConstantLatency final : public LatencyModel
{
 public:
  ConstantLatency(int64_t feed_ns, int64_t order_ns, int64_t fill_ns);

  int64_t feedDelay() override { return _feed_ns; }
  int64_t orderDelay() override { return _order_ns; }
  int64_t fillDelay() override { return _fill_ns; }

 private:
  int64_t _feed_ns;
  int64_t _order_ns;
  int64_t _fill_ns;
};

// Independent normal samples per component, clamped to non-negative.
// Stddev <= 0 collapses the component to a deterministic mean.
class GaussianLatency final : public LatencyModel
{
 public:
  GaussianLatency(double feed_mean_ns, double feed_stddev_ns,
                  double order_mean_ns, double order_stddev_ns,
                  double fill_mean_ns, double fill_stddev_ns,
                  uint64_t seed);

  int64_t feedDelay() override;
  int64_t orderDelay() override;
  int64_t fillDelay() override;

  void reset(uint64_t seed) override { _rng.seed(seed); }

 private:
  int64_t draw(double mean, double stddev);

  std::mt19937_64 _rng;
  double _feed_mean_ns;
  double _feed_stddev_ns;
  double _order_mean_ns;
  double _order_stddev_ns;
  double _fill_mean_ns;
  double _fill_stddev_ns;
};

// Exponential per component, parameterised by mean. Heavy right tail
// is the default for network-bound latency.
class ExponentialLatency final : public LatencyModel
{
 public:
  ExponentialLatency(double feed_mean_ns, double order_mean_ns,
                     double fill_mean_ns, uint64_t seed);

  int64_t feedDelay() override;
  int64_t orderDelay() override;
  int64_t fillDelay() override;

  void reset(uint64_t seed) override { _rng.seed(seed); }

 private:
  int64_t draw(double mean);

  std::mt19937_64 _rng;
  double _feed_mean_ns;
  double _order_mean_ns;
  double _fill_mean_ns;
};

// Resample with replacement from observed values. The arrays are
// owned by this object; pass by move at construction.
class EmpiricalLatency final : public LatencyModel
{
 public:
  EmpiricalLatency(std::vector<int64_t> feed_samples,
                   std::vector<int64_t> order_samples,
                   std::vector<int64_t> fill_samples,
                   uint64_t seed);

  int64_t feedDelay() override;
  int64_t orderDelay() override;
  int64_t fillDelay() override;

  void reset(uint64_t seed) override { _rng.seed(seed); }

 private:
  int64_t draw(const std::vector<int64_t>& samples);

  std::mt19937_64 _rng;
  std::vector<int64_t> _feed_samples;
  std::vector<int64_t> _order_samples;
  std::vector<int64_t> _fill_samples;
};

}  // namespace flox
