#pragma once

#include <concepts>
#include <span>
#include <vector>

// Targets are forward-looking labels (e.g. future return, future volatility).
// They are batch-only and intentionally have no streaming `update()` overload —
// feeding them into a live update loop is a look-ahead-bias bug.
//
// Targets live in `flox::target`, distinct from `flox::indicator`, so the
// lookahead is visible at the call site. The `BatchTarget` concept below is
// the type-level guard: anything that looks like a streaming indicator does
// not satisfy it.

namespace flox::target
{

template <typename T>
concept BatchTarget = requires(const T& t, std::span<const double> input) {
  { t.compute(input) } -> std::same_as<std::vector<double>>;
  { t.horizon() } noexcept -> std::convertible_to<size_t>;
  { T::is_target } -> std::convertible_to<bool>;
} && (T::is_target == true);

}  // namespace flox::target
