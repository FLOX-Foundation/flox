/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/latency_model.h"

#include <algorithm>
#include <stdexcept>

namespace flox
{

namespace
{

void requireNonNegative(int64_t value, const char* name)
{
  if (value < 0)
  {
    throw std::invalid_argument(std::string(name) + " must be non-negative");
  }
}

void requireNonNegative(double value, const char* name)
{
  if (value < 0.0)
  {
    throw std::invalid_argument(std::string(name) + " must be non-negative");
  }
}

}  // namespace

// ── ConstantLatency ─────────────────────────────────────────────────

ConstantLatency::ConstantLatency(int64_t feed_ns, int64_t order_ns, int64_t fill_ns)
    : _feed_ns(feed_ns), _order_ns(order_ns), _fill_ns(fill_ns)
{
  requireNonNegative(feed_ns, "feed_ns");
  requireNonNegative(order_ns, "order_ns");
  requireNonNegative(fill_ns, "fill_ns");
}

// ── GaussianLatency ────────────────────────────────────────────────

GaussianLatency::GaussianLatency(double feed_mean_ns, double feed_stddev_ns,
                                 double order_mean_ns, double order_stddev_ns,
                                 double fill_mean_ns, double fill_stddev_ns,
                                 uint64_t seed)
    : _rng(seed),
      _feed_mean_ns(feed_mean_ns),
      _feed_stddev_ns(feed_stddev_ns),
      _order_mean_ns(order_mean_ns),
      _order_stddev_ns(order_stddev_ns),
      _fill_mean_ns(fill_mean_ns),
      _fill_stddev_ns(fill_stddev_ns)
{
  requireNonNegative(feed_mean_ns, "feed_mean_ns");
  requireNonNegative(feed_stddev_ns, "feed_stddev_ns");
  requireNonNegative(order_mean_ns, "order_mean_ns");
  requireNonNegative(order_stddev_ns, "order_stddev_ns");
  requireNonNegative(fill_mean_ns, "fill_mean_ns");
  requireNonNegative(fill_stddev_ns, "fill_stddev_ns");
}

int64_t GaussianLatency::draw(double mean, double stddev)
{
  if (stddev <= 0.0)
  {
    return std::max<int64_t>(0, static_cast<int64_t>(mean));
  }
  std::normal_distribution<double> dist(mean, stddev);
  double v = dist(_rng);
  return std::max<int64_t>(0, static_cast<int64_t>(v));
}

int64_t GaussianLatency::feedDelay() { return draw(_feed_mean_ns, _feed_stddev_ns); }
int64_t GaussianLatency::orderDelay() { return draw(_order_mean_ns, _order_stddev_ns); }
int64_t GaussianLatency::fillDelay() { return draw(_fill_mean_ns, _fill_stddev_ns); }

// ── ExponentialLatency ─────────────────────────────────────────────

ExponentialLatency::ExponentialLatency(double feed_mean_ns, double order_mean_ns,
                                       double fill_mean_ns, uint64_t seed)
    : _rng(seed),
      _feed_mean_ns(feed_mean_ns),
      _order_mean_ns(order_mean_ns),
      _fill_mean_ns(fill_mean_ns)
{
  requireNonNegative(feed_mean_ns, "feed_mean_ns");
  requireNonNegative(order_mean_ns, "order_mean_ns");
  requireNonNegative(fill_mean_ns, "fill_mean_ns");
}

int64_t ExponentialLatency::draw(double mean)
{
  if (mean <= 0.0)
  {
    return 0;
  }
  std::exponential_distribution<double> dist(1.0 / mean);
  double v = dist(_rng);
  return std::max<int64_t>(0, static_cast<int64_t>(v));
}

int64_t ExponentialLatency::feedDelay() { return draw(_feed_mean_ns); }
int64_t ExponentialLatency::orderDelay() { return draw(_order_mean_ns); }
int64_t ExponentialLatency::fillDelay() { return draw(_fill_mean_ns); }

// ── EmpiricalLatency ───────────────────────────────────────────────

EmpiricalLatency::EmpiricalLatency(std::vector<int64_t> feed_samples,
                                   std::vector<int64_t> order_samples,
                                   std::vector<int64_t> fill_samples,
                                   uint64_t seed)
    : _rng(seed),
      _feed_samples(std::move(feed_samples)),
      _order_samples(std::move(order_samples)),
      _fill_samples(std::move(fill_samples))
{
  if (_feed_samples.empty() && _order_samples.empty() && _fill_samples.empty())
  {
    throw std::invalid_argument(
        "EmpiricalLatency needs at least one of feed/order/fill samples");
  }
  for (auto& arr : {_feed_samples, _order_samples, _fill_samples})
  {
    for (auto v : arr)
    {
      if (v < 0)
      {
        throw std::invalid_argument("empirical samples must be non-negative");
      }
    }
  }
}

int64_t EmpiricalLatency::draw(const std::vector<int64_t>& samples)
{
  if (samples.empty())
  {
    return 0;
  }
  std::uniform_int_distribution<size_t> dist(0, samples.size() - 1);
  return std::max<int64_t>(0, samples[dist(_rng)]);
}

int64_t EmpiricalLatency::feedDelay() { return draw(_feed_samples); }
int64_t EmpiricalLatency::orderDelay() { return draw(_order_samples); }
int64_t EmpiricalLatency::fillDelay() { return draw(_fill_samples); }

}  // namespace flox
