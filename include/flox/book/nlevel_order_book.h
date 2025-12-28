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
#include "flox/util/base/math.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <type_traits>

namespace flox
{

template <size_t MaxLevels = 8192>
class NLevelOrderBook : public IOrderBook
{
 public:
  static constexpr size_t MAX_LEVELS = MaxLevels;

  explicit NLevelOrderBook(Price tickSize)
      : _tickSize(tickSize)
  {
    _tickSizeDiv = math::make_fastdiv64((uint64_t)_tickSize.raw(), 1);
    clear();
  }

  inline std::optional<size_t> bestAskIndex() const
  {
    if (_bestAskIdx < MAX_LEVELS)
    {
      return _bestAskIdx;
    }

    if (_minAsk >= MAX_LEVELS)
    {
      return std::nullopt;
    }

    for (size_t i = _minAsk; i <= _maxAsk && i < MAX_LEVELS; ++i)
    {
      if (!_asks[i].isZero())
      {
        return i;
      }
    }
    return std::nullopt;
  }

  inline std::optional<size_t> bestBidIndex() const
  {
    if (_bestBidIdx < MAX_LEVELS)
    {
      return _bestBidIdx;
    }

    if (_minBid >= MAX_LEVELS)
    {
      return std::nullopt;
    }

    for (size_t i = _maxBid + 1; i-- > _minBid;)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }

      if (i == 0)
      {
        break;
      }
    }
    return std::nullopt;
  }

  void dump(std::ostream& os, size_t levels,
            int pricePrec = 4, int qtyPrec = 3, bool ansi = false) const
  {
    if (levels == 0)
    {
      return;
    }

    if (levels > 512)
    {
      levels = 512;
    }

    auto aBest = bestAsk();
    auto bBest = bestBid();

    const double ts = _tickSize.toDouble();
    os.setf(std::ios::fixed);
    os << "tick=" << std::setprecision(pricePrec) << ts
       << "  baseIndex=" << _baseIndex;

    if (aBest && bBest)
    {
      const double ba = aBest->toDouble(), bb = bBest->toDouble();
      os << "  spread=" << std::setprecision(pricePrec) << (ba - bb)
         << "  mid=" << std::setprecision(pricePrec) << ((ba + bb) * 0.5);
    }
    os << "\n";

    struct Row
    {
      double px{0}, qty{0};
      bool have{false};
    };

    std::array<Row, 512> asks{}, bids{};
    size_t na = 0, nb = 0;

    if (auto aIdxOpt = bestAskIndex())
    {
      for (size_t i = *aIdxOpt; i <= _maxAsk && i < MAX_LEVELS && na < levels; ++i)
      {
        if (_asks[i].isZero())
        {
          continue;
        }

        asks[na].have = true;
        asks[na].px = indexToPrice(i).toDouble();
        asks[na].qty = _asks[i].toDouble();
        ++na;
      }
    }

    if (auto bIdxOpt = bestBidIndex())
    {
      for (size_t i = *bIdxOpt + 1; i-- > _minBid && nb < levels;)
      {
        if (_bids[i].isZero())
        {
          if (i == 0)
          {
            break;
          }
          continue;
        }

        bids[nb].have = true;
        bids[nb].px = indexToPrice(i).toDouble();
        bids[nb].qty = _bids[i].toDouble();
        ++nb;

        if (i == 0)
        {
          break;
        }
      }
    }

    auto num_len = [](double v, int prec) -> int
    {
      char buf[64];
      auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, prec);
      return int(r.ptr - buf);
    };

    auto print_num = [](std::ostream& o, double v, int prec, int width)
    {
      char buf[64];
      auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, prec);
      int len = int(r.ptr - buf);
      for (int i = 0; i < width - len; ++i)
      {
        o.put(' ');
      }
      o.write(buf, len);
    };

    auto print_dash = [](std::ostream& o, int width)
    {
      for (int i = 0; i < width - 1; ++i)
      {
        o.put(' ');
      }
      o.put('-');
    };

    int wQty = 7;
    int wPx = 6;

    for (size_t i = 0; i < na; ++i)
    {
      wQty = std::max(wQty, num_len(asks[i].qty, qtyPrec));
      wPx = std::max(wPx, num_len(asks[i].px, pricePrec));
    }

    for (size_t i = 0; i < nb; ++i)
    {
      wQty = std::max(wQty, num_len(bids[i].qty, qtyPrec));
      wPx = std::max(wPx, num_len(bids[i].px, pricePrec));
    }

    const char* RED = ansi ? "\033[31m" : "";
    const char* GRN = ansi ? "\033[32m" : "";
    const char* DIM = ansi ? "\033[2m" : "";
    const char* RST = ansi ? "\033[0m" : "";

    os << "  " << std::setw(wQty) << "ASK_QTY"
       << "  " << std::setw(wPx) << "ASK_PX"
       << "  " << DIM << "│" << RST << "  "
       << std::setw(wPx) << "BID_PX"
       << "  " << std::setw(wQty) << "BID_QTY"
       << "\n";

    const size_t rows = std::max(na, nb);

    for (size_t r = 0; r < rows; ++r)
    {
      os << "  ";
      if (r < na && asks[r].have)
      {
        os << RED;
        print_num(os, asks[r].qty, qtyPrec, wQty);
        os << RST;
      }
      else
      {
        print_dash(os, wQty);
      }

      os << "  ";
      if (r < na && asks[r].have)
      {
        os << RED;
        print_num(os, asks[r].px, pricePrec, wPx);
        os << RST;
      }
      else
      {
        print_dash(os, wPx);
      }

      os << "  " << DIM << "│" << RST << "  ";

      if (r < nb && bids[r].have)
      {
        os << GRN;
        print_num(os, bids[r].px, pricePrec, wPx);
        os << RST;
      }
      else
      {
        print_dash(os, wPx);
      }

      os << "  ";
      if (r < nb && bids[r].have)
      {
        os << GRN;
        print_num(os, bids[r].qty, qtyPrec, wQty);
        os << RST;
      }
      else
      {
        print_dash(os, wQty);
      }

      os << "\n";
    }
  }

  void applyBookUpdate(const BookUpdateEvent& ev) override
  {
    const auto& up = ev.update;

    if (up.type == BookUpdateType::SNAPSHOT)
    {
      int64_t minIdx = std::numeric_limits<int64_t>::max();
      int64_t maxIdx = std::numeric_limits<int64_t>::min();

      auto acc = [&](const auto& vec)
      {
        for (const auto& [p, _] : vec)
        {
          const int64_t t = ticks(p);
          if (t < minIdx)
          {
            minIdx = t;
          }
          if (t > maxIdx)
          {
            maxIdx = t;
          }
        }
      };

      acc(up.bids);
      acc(up.asks);

      if (minIdx == std::numeric_limits<int64_t>::max())
      {
        clear();
      }
      else
      {
        reanchor(minIdx, maxIdx);
      }

      _bids.fill({});
      _asks.fill({});
      _minBid = _minAsk = MAX_LEVELS;
      _maxBid = _maxAsk = 0;
      _bestBidIdx = _bestAskIdx = MAX_LEVELS;
      _bestBidTick = _bestAskTick = -1;
    }

    for (const auto& [p, q] : up.bids)
    {
      const size_t i = localIndex(p);
      if (i >= MAX_LEVELS)
      {
        continue;
      }

      const bool had = !_bids[i].isZero();
      if (_bids[i].raw() == q.raw())
      {
        continue;
      }

      _bids[i] = q;

      if (!q.isZero())
      {
        if (i < _minBid)
        {
          _minBid = i;
        }
        if (i > _maxBid)
        {
          _maxBid = i;
        }
        if (_bestBidIdx >= MAX_LEVELS || i > _bestBidIdx)
        {
          _bestBidIdx = i;
          _bestBidTick = _baseIndex + static_cast<int64_t>(i);
        }
      }
      else if (had)
      {
        if (i == _bestBidIdx)
        {
          _bestBidIdx = prevNonZeroBid(i);
          _bestBidTick = (_bestBidIdx < MAX_LEVELS)
                             ? (_baseIndex + static_cast<int64_t>(_bestBidIdx))
                             : -1;
        }
        if (i == _minBid)
        {
          _minBid = nextNonZeroBid(_minBid);
        }
        if (i == _maxBid)
        {
          _maxBid = prevNonZeroBid(_maxBid);
        }
      }
    }

    for (const auto& [p, q] : up.asks)
    {
      const size_t i = localIndex(p);
      if (i >= MAX_LEVELS)
      {
        continue;
      }

      const bool had = !_asks[i].isZero();
      if (_asks[i].raw() == q.raw())
      {
        continue;
      }

      _asks[i] = q;

      if (!q.isZero())
      {
        if (i < _minAsk)
        {
          _minAsk = i;
        }
        if (i > _maxAsk)
        {
          _maxAsk = i;
        }
        if (_bestAskIdx >= MAX_LEVELS || i < _bestAskIdx)
        {
          _bestAskIdx = i;
          _bestAskTick = _baseIndex + static_cast<int64_t>(i);
        }
      }
      else if (had)
      {
        if (i == _bestAskIdx)
        {
          _bestAskIdx = nextNonZeroAsk(i);
          _bestAskTick = (_bestAskIdx < MAX_LEVELS)
                             ? (_baseIndex + static_cast<int64_t>(_bestAskIdx))
                             : -1;
        }
        if (i == _minAsk)
        {
          _minAsk = nextNonZeroAsk(_minAsk);
        }
        if (i == _maxAsk)
        {
          _maxAsk = prevNonZeroAsk(_maxAsk);
        }
      }
    }
  }

  inline std::optional<Price> bestBid() const override
  {
    const int64_t t = _bestBidTick;
    if (t < 0)
    {
      return std::nullopt;
    }
    return std::optional<Price>{Price::fromRaw(_tickSize.raw() * t)};
  }

  inline std::optional<Price> bestAsk() const override
  {
    const int64_t t = _bestAskTick;
    if (t < 0)
    {
      return std::nullopt;
    }
    return std::optional<Price>{Price::fromRaw(_tickSize.raw() * t)};
  }

  inline Quantity bidAtPrice(Price p) const override
  {
    const size_t i = localIndex(p);
    return i < MAX_LEVELS ? _bids[i] : Quantity{};
  }

  inline Quantity askAtPrice(Price p) const override
  {
    const size_t i = localIndex(p);
    return i < MAX_LEVELS ? _asks[i] : Quantity{};
  }

  inline std::pair<Quantity, Volume> consumeAsks(Quantity needQty) const
  {
    if (_bestAskIdx >= MAX_LEVELS)
    {
      return {Quantity{}, Volume{}};
    }

#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
    // Fast path: 128-bit accumulator with single division at end (GCC/Clang)
    int64_t remRaw = needQty.raw();
    __int128_t notionalRaw2 = 0;

    size_t i = _bestAskIdx;
    const size_t hi = _maxAsk;
    const int64_t ts = _tickSize.raw();
    int64_t tickNum = _baseIndex + static_cast<int64_t>(i);

    for (; i <= hi && remRaw > 0; ++i, ++tickNum)
    {
      const int64_t qRaw = _asks[i].raw();
      if (qRaw == 0)
      {
        continue;
      }

      const int64_t takeRaw = (qRaw < remRaw) ? qRaw : remRaw;
      notionalRaw2 += static_cast<__int128_t>(takeRaw) * static_cast<__int128_t>(ts * tickNum);
      remRaw -= takeRaw;
    }

    return {Quantity::fromRaw(needQty.raw() - remRaw),
            Volume::fromRaw(static_cast<int64_t>(notionalRaw2 / Volume::Scale))};
#else
    // Fallback: use type-safe operators (slower but portable)
    Quantity remaining = needQty;
    Volume notional{};

    size_t i = _bestAskIdx;
    const size_t hi = _maxAsk;
    int64_t tickNum = _baseIndex + static_cast<int64_t>(i);

    for (; i <= hi && remaining.raw() > 0; ++i, ++tickNum)
    {
      const Quantity available = _asks[i];
      if (available.isZero())
      {
        continue;
      }

      const Quantity take = available.raw() < remaining.raw() ? available : remaining;
      const Price price = Price::fromRaw(_tickSize.raw() * tickNum);
      notional = notional + (take * price);
      remaining = remaining - take;
    }

    return {needQty - remaining, notional};
#endif
  }

  inline std::pair<Quantity, Volume> consumeBids(Quantity needQty) const
  {
    if (_bestBidIdx >= MAX_LEVELS)
    {
      return {Quantity{}, Volume{}};
    }

#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
    // Fast path: 128-bit accumulator with single division at end (GCC/Clang)
    int64_t remRaw = needQty.raw();
    __int128_t notionalRaw2 = 0;

    size_t i = _bestBidIdx;
    const size_t lo = _minBid;
    const int64_t ts = _tickSize.raw();
    int64_t tickNum = _baseIndex + static_cast<int64_t>(i);

    for (;;)
    {
      if (remRaw <= 0)
      {
        break;
      }

      const int64_t qRaw = _bids[i].raw();
      if (qRaw != 0)
      {
        const int64_t takeRaw = (qRaw < remRaw) ? qRaw : remRaw;
        notionalRaw2 += static_cast<__int128_t>(takeRaw) * static_cast<__int128_t>(ts * tickNum);
        remRaw -= takeRaw;
      }

      if (i == lo)
      {
        break;
      }
      --i;
      --tickNum;
    }

    return {Quantity::fromRaw(needQty.raw() - remRaw),
            Volume::fromRaw(static_cast<int64_t>(notionalRaw2 / Volume::Scale))};
#else
    // Fallback: use type-safe operators (slower but portable)
    Quantity remaining = needQty;
    Volume notional{};

    size_t i = _bestBidIdx;
    const size_t lo = _minBid;
    int64_t tickNum = _baseIndex + static_cast<int64_t>(i);

    for (;;)
    {
      if (remaining.raw() <= 0)
      {
        break;
      }

      const Quantity available = _bids[i];
      if (!available.isZero())
      {
        const Quantity take = available.raw() < remaining.raw() ? available : remaining;
        const Price price = Price::fromRaw(_tickSize.raw() * tickNum);
        notional = notional + (take * price);
        remaining = remaining - take;
      }

      if (i == lo)
      {
        break;
      }
      --i;
      --tickNum;
    }

    return {needQty - remaining, notional};
#endif
  }

  Price tickSize() const { return _tickSize; }

  inline bool isCrossed() const
  {
    if (_bestBidTick < 0 || _bestAskTick < 0)
    {
      return false;
    }
    return _bestBidTick >= _bestAskTick;
  }

  inline std::optional<Price> spread() const
  {
    if (_bestBidTick < 0 || _bestAskTick < 0)
    {
      return std::nullopt;
    }
    int64_t spreadTicks = _bestAskTick - _bestBidTick;
    return Price::fromRaw(_tickSize.raw() * spreadTicks);
  }

  inline std::optional<Price> mid() const
  {
    if (_bestBidTick < 0 || _bestAskTick < 0)
    {
      return std::nullopt;
    }
    // Multiply by half-tick to avoid precision loss from integer division
    // midPrice = tickSize * (bidTick + askTick) / 2
    //          = (tickSize / 2) * (bidTick + askTick)
    const int64_t halfTick = _tickSize.raw() / 2;
    const int64_t midTick2 = _bestBidTick + _bestAskTick;
    return Price::fromRaw(halfTick * midTick2);
  }

  struct PriceLevel
  {
    Price price;
    Quantity quantity;
  };

  std::vector<PriceLevel> getBidLevels(size_t maxLevels) const
  {
    std::vector<PriceLevel> levels;
    levels.reserve(maxLevels);

    if (auto bidIdx = bestBidIndex())
    {
      for (size_t i = *bidIdx + 1; levels.size() < maxLevels;)
      {
        if (i-- == 0)
        {
          break;
        }
        if (_bids[i].isZero())
        {
          continue;
        }
        levels.push_back({indexToPrice(i), _bids[i]});
      }
    }
    return levels;
  }

  std::vector<PriceLevel> getAskLevels(size_t maxLevels) const
  {
    std::vector<PriceLevel> levels;
    levels.reserve(maxLevels);

    if (auto askIdx = bestAskIndex())
    {
      for (size_t i = *askIdx; levels.size() < maxLevels && i < MAX_LEVELS; i++)
      {
        if (_asks[i].isZero())
        {
          continue;
        }
        levels.push_back({indexToPrice(i), _asks[i]});
      }
    }
    return levels;
  }

  void clear()
  {
    _bids.fill({});
    _asks.fill({});
    _minBid = _minAsk = MAX_LEVELS;
    _maxBid = _maxAsk = 0;
    _baseIndex = 0;
    _bestBidIdx = _bestAskIdx = MAX_LEVELS;
    _bestBidTick = _bestAskTick = -1;
  }

 private:
  int64_t ticks(Price p) const
  {
    const int64_t pr = p.raw();
    return math::sdiv_round_nearest(pr, _tickSizeDiv);
  }

  Price indexToPrice(size_t i) const
  {
    const int64_t ts = _tickSize.raw();
    const int64_t tick = _baseIndex + static_cast<int64_t>(i);
    return Price::fromRaw(ts * tick);
  }

  size_t localIndex(Price p) const
  {
    const int64_t t = ticks(p) - _baseIndex;
    return (static_cast<uint64_t>(t) < static_cast<uint64_t>(MAX_LEVELS))
               ? static_cast<size_t>(t)
               : MAX_LEVELS;
  }

  void reanchor(int64_t minIdx, int64_t maxIdx)
  {
    constexpr int64_t HYST = 8;
    const int64_t span = maxIdx - minIdx + 1;
    const int64_t curLo = _baseIndex;
    const int64_t curHi = _baseIndex + static_cast<int64_t>(MAX_LEVELS) - 1;

    if (curLo + HYST <= minIdx && maxIdx <= curHi - HYST)
    {
      return;
    }

    if (span >= static_cast<int64_t>(MAX_LEVELS))
    {
      _baseIndex = minIdx;
    }
    else
    {
      const int64_t mid = (minIdx + maxIdx) / 2;
      _baseIndex = mid - static_cast<int64_t>(MAX_LEVELS / 2);
    }
  }

  void reanchorWithData(int64_t minIdx, int64_t maxIdx)
  {
    const int64_t oldBase = _baseIndex;

    // Calculate new base index
    const int64_t span = maxIdx - minIdx + 1;
    int64_t newBase;
    if (span >= static_cast<int64_t>(MAX_LEVELS))
    {
      newBase = minIdx;
    }
    else
    {
      const int64_t mid = (minIdx + maxIdx) / 2;
      newBase = mid - static_cast<int64_t>(MAX_LEVELS / 2);
    }

    if (newBase == oldBase)
    {
      return;
    }

    const int64_t shift = oldBase - newBase;

    // Create temporary copies and shift data
    std::array<Quantity, MAX_LEVELS> newBids{};
    std::array<Quantity, MAX_LEVELS> newAsks{};

    // Copy bids with offset
    if (_minBid < MAX_LEVELS)
    {
      for (size_t i = _minBid; i <= _maxBid && i < MAX_LEVELS; ++i)
      {
        if (_bids[i].isZero())
        {
          continue;
        }
        const int64_t newIdx = static_cast<int64_t>(i) + shift;
        if (newIdx >= 0 && newIdx < static_cast<int64_t>(MAX_LEVELS))
        {
          newBids[static_cast<size_t>(newIdx)] = _bids[i];
        }
      }
    }

    // Copy asks with offset
    if (_minAsk < MAX_LEVELS)
    {
      for (size_t i = _minAsk; i <= _maxAsk && i < MAX_LEVELS; ++i)
      {
        if (_asks[i].isZero())
        {
          continue;
        }
        const int64_t newIdx = static_cast<int64_t>(i) + shift;
        if (newIdx >= 0 && newIdx < static_cast<int64_t>(MAX_LEVELS))
        {
          newAsks[static_cast<size_t>(newIdx)] = _asks[i];
        }
      }
    }

    _bids = std::move(newBids);
    _asks = std::move(newAsks);
    _baseIndex = newBase;

    // Recalculate min/max/best indices
    _minBid = _minAsk = MAX_LEVELS;
    _maxBid = _maxAsk = 0;
    _bestBidIdx = _bestAskIdx = MAX_LEVELS;
    _bestBidTick = _bestAskTick = -1;

    for (size_t i = 0; i < MAX_LEVELS; ++i)
    {
      if (!_bids[i].isZero())
      {
        if (i < _minBid)
        {
          _minBid = i;
        }
        if (i > _maxBid)
        {
          _maxBid = i;
        }
        if (_bestBidIdx >= MAX_LEVELS || i > _bestBidIdx)
        {
          _bestBidIdx = i;
          _bestBidTick = newBase + static_cast<int64_t>(i);
        }
      }
      if (!_asks[i].isZero())
      {
        if (i < _minAsk)
        {
          _minAsk = i;
        }
        if (i > _maxAsk)
        {
          _maxAsk = i;
        }
        if (_bestAskIdx >= MAX_LEVELS || i < _bestAskIdx)
        {
          _bestAskIdx = i;
          _bestAskTick = newBase + static_cast<int64_t>(i);
        }
      }
    }
  }

  size_t nextNonZeroAsk(size_t from) const
  {
    // Limit search to known ask range
    const size_t limit = (_maxAsk < MAX_LEVELS) ? _maxAsk + 1 : MAX_LEVELS;
    for (size_t i = from; i < limit; ++i)
    {
      if (!_asks[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

  size_t prevNonZeroAsk(size_t from) const
  {
    // Limit search to known ask range
    const size_t limit = (_minAsk < MAX_LEVELS) ? _minAsk : 0;
    if (from < limit)
    {
      return MAX_LEVELS;
    }
    for (size_t i = from + 1; i-- > limit;)
    {
      if (!_asks[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

  size_t nextNonZeroBid(size_t from) const
  {
    // Limit search to known bid range
    const size_t limit = (_maxBid < MAX_LEVELS) ? _maxBid + 1 : MAX_LEVELS;
    for (size_t i = from; i < limit; ++i)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

  size_t prevNonZeroBid(size_t from) const
  {
    // Limit search to known bid range
    const size_t limit = (_minBid < MAX_LEVELS) ? _minBid : 0;
    if (from < limit)
    {
      return MAX_LEVELS;
    }
    for (size_t i = from + 1; i-- > limit;)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

 private:
  Price _tickSize;
  math::FastDiv64 _tickSizeDiv;

  int64_t _baseIndex{0};

  alignas(64) std::array<Quantity, MAX_LEVELS> _bids{};
  alignas(64) std::array<Quantity, MAX_LEVELS> _asks{};

  size_t _minBid{MAX_LEVELS}, _maxBid{0}, _minAsk{MAX_LEVELS}, _maxAsk{0};

  size_t _bestBidIdx{MAX_LEVELS}, _bestAskIdx{MAX_LEVELS};
  int64_t _bestBidTick{-1}, _bestAskTick{-1};
};

}  // namespace flox
