/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar_matrix.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/aggregator/timeframe.h"
#include "flox/strategy/strategy.h"

#include <optional>

namespace flox
{

template <size_t MaxTimeframes = 4>
class BarStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  void setBarMatrix(BarMatrix<>* matrix) noexcept { _matrix = matrix; }

  const Bar* bar(SymbolId sym, TimeframeId tf, size_t idx = 0) const noexcept
  {
    return _matrix ? _matrix->bar(sym, tf, idx) : nullptr;
  }

  const Bar* bar(TimeframeId tf, size_t idx = 0) const noexcept
  {
    return bar(symbol(), tf, idx);
  }

  std::optional<Price> close(SymbolId sym, TimeframeId tf, size_t idx = 0) const noexcept
  {
    const auto* b = bar(sym, tf, idx);
    return b ? std::optional<Price>(b->close) : std::nullopt;
  }

  std::optional<Price> close(TimeframeId tf, size_t idx = 0) const noexcept
  {
    return close(symbol(), tf, idx);
  }

  std::optional<Price> open(SymbolId sym, TimeframeId tf, size_t idx = 0) const noexcept
  {
    const auto* b = bar(sym, tf, idx);
    return b ? std::optional<Price>(b->open) : std::nullopt;
  }

  std::optional<Price> open(TimeframeId tf, size_t idx = 0) const noexcept
  {
    return open(symbol(), tf, idx);
  }

  std::optional<Price> high(SymbolId sym, TimeframeId tf, size_t idx = 0) const noexcept
  {
    const auto* b = bar(sym, tf, idx);
    return b ? std::optional<Price>(b->high) : std::nullopt;
  }

  std::optional<Price> high(TimeframeId tf, size_t idx = 0) const noexcept
  {
    return high(symbol(), tf, idx);
  }

  std::optional<Price> low(SymbolId sym, TimeframeId tf, size_t idx = 0) const noexcept
  {
    const auto* b = bar(sym, tf, idx);
    return b ? std::optional<Price>(b->low) : std::nullopt;
  }

  std::optional<Price> low(TimeframeId tf, size_t idx = 0) const noexcept
  {
    return low(symbol(), tf, idx);
  }

  std::optional<Volume> volume(SymbolId sym, TimeframeId tf, size_t idx = 0) const noexcept
  {
    const auto* b = bar(sym, tf, idx);
    return b ? std::optional<Volume>(b->volume) : std::nullopt;
  }

  std::optional<Volume> volume(TimeframeId tf, size_t idx = 0) const noexcept
  {
    return volume(symbol(), tf, idx);
  }

  bool hasBar(SymbolId sym, TimeframeId tf, size_t idx = 0) const noexcept
  {
    return bar(sym, tf, idx) != nullptr;
  }

  bool hasBar(TimeframeId tf, size_t idx = 0) const noexcept
  {
    return hasBar(symbol(), tf, idx);
  }

 private:
  BarMatrix<>* _matrix = nullptr;
};

}  // namespace flox
