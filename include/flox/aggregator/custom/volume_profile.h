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
#include <numeric>

namespace flox
{

// Volume Profile aggregator for market analysis.
// Tracks volume distribution across price levels. Provides POC, Value Area, delta.
template <size_t MaxLevels = 256>
class VolumeProfile
{
 public:
  static constexpr size_t kMaxLevels = MaxLevels;
  static constexpr double kValueAreaPercent = 0.70;

  struct Level
  {
    Price price{};
    Volume volume{};
    Volume buyVolume{};

    Volume sellVolume() const noexcept
    {
      return Volume::fromRaw(volume.raw() - buyVolume.raw());
    }

    Volume delta() const noexcept
    {
      return Volume::fromRaw(buyVolume.raw() - sellVolume().raw());
    }
  };

  VolumeProfile() = default;

  void setTickSize(Price tickSize) noexcept { _tickSize = tickSize; }

  void addTrade(const TradeEvent& trade) noexcept
  {
    const auto notional = trade.trade.quantity * trade.trade.price;

    const auto quantizedPrice = quantize(trade.trade.price);
    auto* level = findOrCreateLevel(quantizedPrice);
    if (!level)
    {
      return;  // Profile full
    }

    level->volume += notional;
    if (trade.trade.isBuy)
    {
      level->buyVolume += notional;
    }

    _totalVolume += notional;
  }

  [[nodiscard]] Price poc() const noexcept
  {
    if (_numLevels == 0)
    {
      return Price{};
    }

    const Level* maxLevel = &_levels[0];
    for (size_t i = 1; i < _numLevels; ++i)
    {
      if (_levels[i].volume.raw() > maxLevel->volume.raw())
      {
        maxLevel = &_levels[i];
      }
    }
    return maxLevel->price;
  }

  [[nodiscard]] Price valueAreaHigh() const
  {
    auto [low, high] = calculateValueArea();
    return high;
  }

  [[nodiscard]] Price valueAreaLow() const
  {
    auto [low, high] = calculateValueArea();
    return low;
  }

  Volume totalVolume() const noexcept { return _totalVolume; }

  Volume totalDelta() const noexcept
  {
    int64_t delta = 0;
    for (size_t i = 0; i < _numLevels; ++i)
    {
      delta += _levels[i].delta().raw();
    }
    return Volume::fromRaw(delta);
  }

  size_t numLevels() const noexcept { return _numLevels; }

  const Level* level(size_t idx) const noexcept
  {
    return idx < _numLevels ? &_levels[idx] : nullptr;
  }

  Volume volumeAt(Price price) const noexcept
  {
    const auto quantizedPrice = quantize(price);
    for (size_t i = 0; i < _numLevels; ++i)
    {
      if (_levels[i].price == quantizedPrice)
      {
        return _levels[i].volume;
      }
    }
    return Volume{};
  }

  void clear() noexcept
  {
    _numLevels = 0;
    _totalVolume = Volume{};
  }

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
    // Linear search (could optimize with binary search for large profiles)
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
    newLevel.volume = Volume{};
    newLevel.buyVolume = Volume{};
    return &newLevel;
  }

  std::pair<Price, Price> calculateValueArea() const
  {
    if (_numLevels == 0)
    {
      return {Price{}, Price{}};
    }

    // Sort indices by price for iteration
    std::array<size_t, MaxLevels> indices;
    std::iota(indices.begin(), indices.begin() + _numLevels, 0);
    std::sort(indices.begin(), indices.begin() + _numLevels,
              [this](size_t a, size_t b)
              { return _levels[a].price.raw() < _levels[b].price.raw(); });

    // Find POC index in sorted array
    size_t pocIdx = 0;
    int64_t maxVol = 0;
    for (size_t i = 0; i < _numLevels; ++i)
    {
      if (_levels[indices[i]].volume.raw() > maxVol)
      {
        maxVol = _levels[indices[i]].volume.raw();
        pocIdx = i;
      }
    }

    // Expand from POC until we have 70% of volume
    const int64_t targetVolume = static_cast<int64_t>(_totalVolume.raw() * kValueAreaPercent);
    int64_t currentVolume = _levels[indices[pocIdx]].volume.raw();
    size_t lowIdx = pocIdx;
    size_t highIdx = pocIdx;

    while (currentVolume < targetVolume && (lowIdx > 0 || highIdx < _numLevels - 1))
    {
      const int64_t lowVol = (lowIdx > 0) ? _levels[indices[lowIdx - 1]].volume.raw() : 0;
      const int64_t highVol = (highIdx < _numLevels - 1) ? _levels[indices[highIdx + 1]].volume.raw() : 0;

      if (lowVol >= highVol && lowIdx > 0)
      {
        --lowIdx;
        currentVolume += lowVol;
      }
      else if (highIdx < _numLevels - 1)
      {
        ++highIdx;
        currentVolume += highVol;
      }
      else if (lowIdx > 0)
      {
        --lowIdx;
        currentVolume += lowVol;
      }
      else
      {
        break;
      }
    }

    return {_levels[indices[lowIdx]].price, _levels[indices[highIdx]].price};
  }

  std::array<Level, MaxLevels> _levels{};
  size_t _numLevels = 0;
  Volume _totalVolume{};
  Price _tickSize{Price::fromRaw(1)};
};

}  // namespace flox
