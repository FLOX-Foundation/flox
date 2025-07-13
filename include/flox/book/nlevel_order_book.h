/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/book/abstract_order_book.h"
#include "flox/book/events/book_update_event.h"
#include "flox/common.h"

#include <array>
#include <cstddef>
#include <optional>

namespace flox
{

template <size_t MaxLevels = 8192>
class NLevelOrderBook : public IOrderBook
{
 public:
  static constexpr size_t MAX_LEVELS = MaxLevels;

  explicit NLevelOrderBook(Price tickSize)
      : _tickSize(tickSize),
        _minBidIndex(MAX_LEVELS),
        _maxBidIndex(0),
        _minAskIndex(MAX_LEVELS),
        _maxAskIndex(0)
  {
    static_assert(MAX_LEVELS < std::numeric_limits<size_t>::max());
  }

  void applyBookUpdate(const BookUpdateEvent& event) override
  {
    const auto& update = event.update;

    if (update.type == BookUpdateType::SNAPSHOT)
    {
      _bids.fill({});
      _asks.fill({});
      _minBidIndex = MAX_LEVELS;
      _maxBidIndex = 0;
      _minAskIndex = MAX_LEVELS;
      _maxAskIndex = 0;
    }

    for (const auto& [price, qty] : update.bids)
    {
      const size_t i = priceToIndex(price);
      if (i >= MAX_LEVELS)
        continue;

      _bids[i] = qty;
      if (qty.isZero())
        continue;

      _minBidIndex = std::min(_minBidIndex, i);
      _maxBidIndex = std::max(_maxBidIndex, i);
    }

    for (const auto& [price, qty] : update.asks)
    {
      const size_t i = priceToIndex(price);
      if (i >= MAX_LEVELS)
        continue;

      _asks[i] = qty;
      if (qty.isZero())
        continue;

      _minAskIndex = std::min(_minAskIndex, i);
      _maxAskIndex = std::max(_maxAskIndex, i);
    }
  }

  std::optional<Price> bestBid() const override
  {
    if (_minBidIndex >= MAX_LEVELS)
      return std::nullopt;

    for (size_t i = _maxBidIndex + 1; i-- > _minBidIndex;)
    {
      if (!_bids[i].isZero())
      {
        return indexToPrice(i);
      }
    }
    return std::nullopt;
  }

  std::optional<Price> bestAsk() const override
  {
    if (_minAskIndex >= MAX_LEVELS)
      return std::nullopt;

    for (size_t i = _minAskIndex; i <= _maxAskIndex && i < MAX_LEVELS; ++i)
    {
      if (!_asks[i].isZero())
      {
        return indexToPrice(i);
      }
    }
    return std::nullopt;
  }

  Quantity bidAtPrice(Price price) const override
  {
    const size_t i = priceToIndex(price);
    return i < MAX_LEVELS ? _bids[i] : Quantity{};
  }

  Quantity askAtPrice(Price price) const override
  {
    const size_t i = priceToIndex(price);
    return i < MAX_LEVELS ? _asks[i] : Quantity{};
  }

  void clear()
  {
    _bids.fill({});
    _asks.fill({});
    _minBidIndex = MAX_LEVELS;
    _maxBidIndex = 0;
    _minAskIndex = MAX_LEVELS;
    _maxAskIndex = 0;
  }

 private:
  size_t priceToIndex(Price price) const
  {
    return static_cast<size_t>((price / _tickSize.raw()).raw());
  }

  Price indexToPrice(size_t index) const
  {
    return _tickSize * static_cast<int64_t>(index);
  }

  Price _tickSize;

  std::array<Quantity, MAX_LEVELS> _bids{};
  std::array<Quantity, MAX_LEVELS> _asks{};

  size_t _minBidIndex;
  size_t _maxBidIndex;
  size_t _minAskIndex;
  size_t _maxAskIndex;
};

}  // namespace flox
