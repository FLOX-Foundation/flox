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
#include <bitset>
#include <chrono>
#include <cstddef>

namespace flox
{

// Market Profile (TPO - Time Price Opportunity) aggregator.
// Tracks price activity across time periods, building a distribution profile.
// Each time period (typically 30 min) gets a letter (A, B, C...).
// Useful for identifying value areas, single prints, poor highs/lows.
template <size_t MaxLevels = 256, size_t MaxPeriods = 26>
class MarketProfile
{
 public:
  static constexpr size_t kMaxLevels = MaxLevels;
  static constexpr size_t kMaxPeriods = MaxPeriods;

  struct Level
  {
    Price price{};
    std::bitset<MaxPeriods> tpos;  // Which periods traded at this price
    uint32_t tpoCount = 0;         // Number of periods with activity

    bool hasPeriod(size_t period) const noexcept
    {
      return period < MaxPeriods && tpos.test(period);
    }

    bool isSinglePrint() const noexcept { return tpoCount == 1; }
  };

  MarketProfile() = default;

  void setTickSize(Price tickSize) noexcept { _tickSize = tickSize; }

  void setPeriodDuration(std::chrono::minutes duration) noexcept
  {
    _periodDurationNs = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  }

  void setSessionStart(uint64_t sessionStartNs) noexcept { _sessionStartNs = sessionStartNs; }

  void addTrade(const TradeEvent& trade) noexcept
  {
    const size_t period = calculatePeriod(trade.trade.exchangeTsNs);
    if (period >= MaxPeriods)
    {
      return;  // Beyond max periods
    }

    const auto quantizedPrice = quantize(trade.trade.price);
    auto* level = findOrCreateLevel(quantizedPrice);
    if (!level)
    {
      return;  // Profile full
    }

    if (!level->tpos.test(period))
    {
      level->tpos.set(period);
      ++level->tpoCount;
    }

    // Track high/low for the session
    if (_numLevels == 1 || quantizedPrice.raw() > _highPrice.raw())
    {
      _highPrice = quantizedPrice;
    }
    if (_numLevels == 1 || quantizedPrice.raw() < _lowPrice.raw())
    {
      _lowPrice = quantizedPrice;
    }

    _currentPeriod = std::max(_currentPeriod, period);
  }

  // Point of Control - price with most TPOs
  Price poc() const noexcept
  {
    if (_numLevels == 0)
    {
      return Price{};
    }

    const Level* maxLevel = &_levels[0];
    for (size_t i = 1; i < _numLevels; ++i)
    {
      if (_levels[i].tpoCount > maxLevel->tpoCount)
      {
        maxLevel = &_levels[i];
      }
    }
    return maxLevel->price;
  }

  // Value Area High - upper bound of 70% TPO concentration
  Price valueAreaHigh() const
  {
    auto [low, high] = calculateValueArea();
    return high;
  }

  // Value Area Low - lower bound of 70% TPO concentration
  Price valueAreaLow() const
  {
    auto [low, high] = calculateValueArea();
    return low;
  }

  // Initial Balance High - high of first two periods (A+B)
  Price initialBalanceHigh() const noexcept
  {
    Price ibHigh{};
    for (size_t i = 0; i < _numLevels; ++i)
    {
      if (_levels[i].hasPeriod(0) || _levels[i].hasPeriod(1))
      {
        if (ibHigh.raw() == 0 || _levels[i].price.raw() > ibHigh.raw())
        {
          ibHigh = _levels[i].price;
        }
      }
    }
    return ibHigh;
  }

  // Initial Balance Low - low of first two periods (A+B)
  Price initialBalanceLow() const noexcept
  {
    Price ibLow{};
    for (size_t i = 0; i < _numLevels; ++i)
    {
      if (_levels[i].hasPeriod(0) || _levels[i].hasPeriod(1))
      {
        if (ibLow.raw() == 0 || _levels[i].price.raw() < ibLow.raw())
        {
          ibLow = _levels[i].price;
        }
      }
    }
    return ibLow;
  }

  Price highPrice() const noexcept { return _highPrice; }
  Price lowPrice() const noexcept { return _lowPrice; }

  size_t numLevels() const noexcept { return _numLevels; }
  size_t currentPeriod() const noexcept { return _currentPeriod; }

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

  // Find single prints (potential support/resistance)
  std::pair<size_t, std::array<Price, MaxLevels>> singlePrints() const noexcept
  {
    std::array<Price, MaxLevels> result{};
    size_t count = 0;

    for (size_t i = 0; i < _numLevels && count < MaxLevels; ++i)
    {
      if (_levels[i].isSinglePrint())
      {
        result[count++] = _levels[i].price;
      }
    }
    return {count, result};
  }

  // Check if price is a poor high (single TPO at extreme)
  bool isPoorHigh() const noexcept
  {
    const auto* highLevel = levelAt(_highPrice);
    return highLevel && highLevel->isSinglePrint();
  }

  // Check if price is a poor low (single TPO at extreme)
  bool isPoorLow() const noexcept
  {
    const auto* lowLevel = levelAt(_lowPrice);
    return lowLevel && lowLevel->isSinglePrint();
  }

  // Get TPO count at a specific price
  uint32_t tpoCountAt(Price price) const noexcept
  {
    const auto* lvl = levelAt(price);
    return lvl ? lvl->tpoCount : 0;
  }

  // Get letter for period (A=0, B=1, etc.)
  static char periodLetter(size_t period) noexcept
  {
    return period < 26 ? static_cast<char>('A' + period) : '?';
  }

  void clear() noexcept
  {
    _numLevels = 0;
    _currentPeriod = 0;
    _highPrice = Price{};
    _lowPrice = Price{};
  }

 private:
  size_t calculatePeriod(uint64_t tradeNs) const noexcept
  {
    if (_periodDurationNs == 0 || tradeNs < _sessionStartNs)
    {
      return 0;
    }
    return static_cast<size_t>((tradeNs - _sessionStartNs) / _periodDurationNs);
  }

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
    newLevel.tpos.reset();
    newLevel.tpoCount = 0;
    return &newLevel;
  }

  std::pair<Price, Price> calculateValueArea() const
  {
    if (_numLevels == 0)
    {
      return {Price{}, Price{}};
    }

    // Sort indices by price
    std::array<size_t, MaxLevels> indices;
    for (size_t i = 0; i < _numLevels; ++i)
    {
      indices[i] = i;
    }
    std::sort(indices.begin(), indices.begin() + _numLevels,
              [this](size_t a, size_t b)
              { return _levels[a].price.raw() < _levels[b].price.raw(); });

    // Calculate total TPOs
    uint32_t totalTpos = 0;
    for (size_t i = 0; i < _numLevels; ++i)
    {
      totalTpos += _levels[i].tpoCount;
    }

    // Find POC index in sorted array
    size_t pocIdx = 0;
    uint32_t maxTpos = 0;
    for (size_t i = 0; i < _numLevels; ++i)
    {
      if (_levels[indices[i]].tpoCount > maxTpos)
      {
        maxTpos = _levels[indices[i]].tpoCount;
        pocIdx = i;
      }
    }

    // Expand from POC until we have 70% of TPOs
    const uint32_t targetTpos = static_cast<uint32_t>(totalTpos * 0.70);
    uint32_t currentTpos = _levels[indices[pocIdx]].tpoCount;
    size_t lowIdx = pocIdx;
    size_t highIdx = pocIdx;

    while (currentTpos < targetTpos && (lowIdx > 0 || highIdx < _numLevels - 1))
    {
      const uint32_t lowTpos = (lowIdx > 0) ? _levels[indices[lowIdx - 1]].tpoCount : 0;
      const uint32_t highTpos = (highIdx < _numLevels - 1) ? _levels[indices[highIdx + 1]].tpoCount : 0;

      if (lowTpos >= highTpos && lowIdx > 0)
      {
        --lowIdx;
        currentTpos += lowTpos;
      }
      else if (highIdx < _numLevels - 1)
      {
        ++highIdx;
        currentTpos += highTpos;
      }
      else if (lowIdx > 0)
      {
        --lowIdx;
        currentTpos += lowTpos;
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
  size_t _currentPeriod = 0;
  Price _highPrice{};
  Price _lowPrice{};
  Price _tickSize{Price::fromRaw(1)};
  uint64_t _periodDurationNs = 30ULL * 60 * 1'000'000'000;  // 30 min default
  uint64_t _sessionStartNs = 0;
};

}  // namespace flox
