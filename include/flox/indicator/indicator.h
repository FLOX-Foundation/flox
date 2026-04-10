#pragma once

#include "flox/aggregator/bar.h"
#include "flox/common.h"

#include <concepts>
#include <span>
#include <vector>

namespace flox::indicator
{

template <typename T>
concept SingleIndicator = requires(const T& ind, std::span<const double> input) {
  { ind.compute(input) } -> std::same_as<std::vector<double>>;
  { ind.period() } noexcept -> std::convertible_to<size_t>;
};

template <typename T>
concept MultiOutputIndicator = requires(const T& ind, std::span<const double> input) {
  { ind.compute(input) };
  { ind.period() } noexcept -> std::convertible_to<size_t>;
};

template <typename T>
concept BarIndicator = requires(const T& ind, std::span<const Bar> bars) {
  { ind.compute(bars) };
  { ind.period() } noexcept -> std::convertible_to<size_t>;
};

template <typename T>
concept HasPeriod = requires(const T& ind) {
  { ind.period() } noexcept -> std::convertible_to<size_t>;
};

inline std::vector<Price> toPrices(const std::vector<double>& values)
{
  std::vector<Price> out(values.size());
  for (size_t i = 0; i < values.size(); ++i)
  {
    out[i] = std::isnan(values[i]) ? Price{} : Price::fromDouble(values[i]);
  }
  return out;
}

inline std::vector<Volume> toVolumes(const std::vector<double>& values)
{
  std::vector<Volume> out(values.size());
  for (size_t i = 0; i < values.size(); ++i)
  {
    out[i] = std::isnan(values[i]) ? Volume{} : Volume::fromDouble(values[i]);
  }
  return out;
}

}  // namespace flox::indicator
