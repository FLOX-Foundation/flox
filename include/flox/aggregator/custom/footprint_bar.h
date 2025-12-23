/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/book/events/trade_event.h"
#include "flox/common.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace flox
{

// Footprint bar for order flow analysis.
// Tracks bid/ask volume at each price level within a bar.
template <size_t MaxLevels = 64>
class FootprintBar
{
 public:
  static constexpr size_t kMaxLevels = MaxLevels;

  struct Level
  {
    Price price{};
    Quantity bidVolume{};  // Sell market orders hitting bids (aggressive sellers)
    Quantity askVolume{};  // Buy market orders hitting asks (aggressive buyers)

    Quantity totalVolume() const noexcept
    {
      return Quantity::fromRaw(bidVolume.raw() + askVolume.raw());
    }

    Quantity delta() const noexcept
    {
      return Quantity::fromRaw(askVolume.raw() - bidVolume.raw());
    }

    double imbalanceRatio() const noexcept
    {
      const auto total = totalVolume().raw();
      if (total == 0)
      {
        return 0.0;
      }
      return static_cast<double>(askVolume.raw()) / static_cast<double>(total);
    }
  };

  FootprintBar() = default;

  void setTickSize(Price tickSize) noexcept { _tickSize = tickSize; }

  void addTrade(const TradeEvent& trade) noexcept
  {
    const auto quantizedPrice = quantize(trade.trade.price);
    auto* level = findOrCreateLevel(quantizedPrice);
    if (!level)
    {
      return;  // Footprint full
    }

    if (trade.trade.isBuy)
    {
      // Buy = aggressive buyer lifting ask
      level->askVolume += trade.trade.quantity;
    }
    else
    {
      // Sell = aggressive seller hitting bid
      level->bidVolume += trade.trade.quantity;
    }
  }

  Quantity totalDelta() const noexcept
  {
    int64_t delta = 0;
    for (size_t i = 0; i < _numLevels; ++i)
    {
      delta += _levels[i].delta().raw();
    }
    return Quantity::fromRaw(delta);
  }

  Quantity totalVolume() const noexcept
  {
    int64_t vol = 0;
    for (size_t i = 0; i < _numLevels; ++i)
    {
      vol += _levels[i].totalVolume().raw();
    }
    return Quantity::fromRaw(vol);
  }

  size_t numLevels() const noexcept { return _numLevels; }

  const Level* level(size_t idx) const noexcept
  {
    return idx < _numLevels ? &_levels[idx] : nullptr;
  }

  const Level* levelAt(Price price) const noexcept
  {
    const auto quantizedPrice = quantize(price);
    for (size_t i = 0; i < _numLevels; ++i)
    {
      if (_levels[i].price == quantizedPrice)
      {
        return &_levels[i];
      }
    }
    return nullptr;
  }

  Price highestBuyingPressure() const noexcept
  {
    if (_numLevels == 0)
    {
      return Price{};
    }

    const Level* best = &_levels[0];
    for (size_t i = 1; i < _numLevels; ++i)
    {
      if (_levels[i].askVolume.raw() > best->askVolume.raw())
      {
        best = &_levels[i];
      }
    }
    return best->price;
  }

  Price highestSellingPressure() const noexcept
  {
    if (_numLevels == 0)
    {
      return Price{};
    }

    const Level* best = &_levels[0];
    for (size_t i = 1; i < _numLevels; ++i)
    {
      if (_levels[i].bidVolume.raw() > best->bidVolume.raw())
      {
        best = &_levels[i];
      }
    }
    return best->price;
  }

  // Returns price of strongest imbalance (ratio >= threshold), or empty Price if none
  Price strongestImbalance(double threshold = 0.7) const noexcept
  {
    Price result{};
    double maxImbalance = 0.0;

    for (size_t i = 0; i < _numLevels; ++i)
    {
      const auto& lvl = _levels[i];
      if (lvl.totalVolume().raw() == 0)
      {
        continue;
      }

      const double ratio = lvl.imbalanceRatio();
      const double imbalance = std::abs(ratio - 0.5) * 2.0;  // Normalize to 0-1

      if (imbalance > maxImbalance && imbalance >= (threshold - 0.5) * 2.0)
      {
        maxImbalance = imbalance;
        result = lvl.price;
      }
    }
    return result;
  }

  void clear() noexcept { _numLevels = 0; }

 private:
  Price quantize(Price price) const noexcept
  {
    if (_tickSize.raw() == 0)
    {
      return price;
    }
    const auto ticks = price.raw() / _tickSize.raw();
    return Price::fromRaw(ticks * _tickSize.raw());
  }

  Level* findOrCreateLevel(Price price) noexcept
  {
    for (size_t i = 0; i < _numLevels; ++i)
    {
      if (_levels[i].price == price)
      {
        return &_levels[i];
      }
    }

    if (_numLevels >= MaxLevels)
    {
      return nullptr;
    }

    auto& newLevel = _levels[_numLevels++];
    newLevel.price = price;
    newLevel.bidVolume = Quantity{};
    newLevel.askVolume = Quantity{};
    return &newLevel;
  }

  std::array<Level, MaxLevels> _levels{};
  size_t _numLevels = 0;
  Price _tickSize{Price::fromRaw(1)};
};

}  // namespace flox
